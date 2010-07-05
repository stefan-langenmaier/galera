//
// Copyright (C) 2010 Codership Oy <info@codership.com>
//


#include "replicator_smm.hpp"
#include "uuid.hpp"
#include "serialization.hpp"

extern "C"
{
#include "galera_info.h"
}

#include <fstream>
#include <sstream>
#include <iostream>


static wsrep_status_t apply_statement(void* recv_ctx,
                                      wsrep_bf_apply_cb_t apply_cb,
                                      const char* query,
                                      size_t len,
                                      wsrep_seqno_t seqno_g)
{
    assert(seqno_g > 0);
    assert(apply_cb != 0);

    wsrep_apply_data_t data;
    data.type           = WSREP_APPLY_SQL;
    data.u.sql.stm      = query;
    data.u.sql.len      = strlen (data.u.sql.stm) + 1; // terminating 0
    data.u.sql.timeval  = static_cast<time_t>(0);
    data.u.sql.randseed = 0;
    return apply_cb(recv_ctx, &data, seqno_g);
}


static wsrep_status_t apply_ws(void* recv_ctx,
                               wsrep_bf_apply_cb_t apply_cb,
                               const galera::WriteSet& ws,
                               wsrep_seqno_t seqno_g)
{
    assert(seqno_g > 0);
    assert(apply_cb != 0);
    using galera::WriteSet;
    using galera::StatementSequence;

    wsrep_status_t retval(WSREP_OK);
    switch (ws.get_level())
    {
    case WriteSet::L_DATA:
    {
        wsrep_apply_data_t data;
        data.type = WSREP_APPLY_APP;
        data.u.app.buffer = const_cast<uint8_t*>(&ws.get_data()[0]);
        data.u.app.len    = ws.get_data().size();
        retval = apply_cb(recv_ctx, &data, seqno_g);
        break;
    }
    case WriteSet::L_STATEMENT:
    {
        const StatementSequence& ss(ws.get_queries());
        for (StatementSequence::const_iterator i = ss.begin();
             i != ss.end(); ++i)
        {
            wsrep_apply_data_t data;
            data.type      = WSREP_APPLY_SQL;
            data.u.sql.stm = reinterpret_cast<const char*>(&i->get_query()[0]);
            data.u.sql.len      = i->get_query().size();
            data.u.sql.timeval  = i->get_tstamp();
            data.u.sql.randseed = i->get_rnd_seed();
            switch ((retval = apply_cb(recv_ctx, &data, seqno_g)))
            {
            case WSREP_OK: break;
            case WSREP_NOT_IMPLEMENTED:
                log_warn << "bf applier returned not implemented for " << *i;
                break;
            default:
                log_error << "apply failed for " << *i;
                retval = WSREP_FATAL;
                break;
            }
        }
        break;
    }

    default:
        log_warn << "data replication level " << ws.get_level()
                 << " not supported";
        retval = WSREP_TRX_FAIL;
    }
    return retval;
}



static wsrep_status_t apply_wscoll(void* recv_ctx,
                                   wsrep_bf_apply_cb_t apply_cb,
                                   const galera::TrxHandle& trx)
{
    wsrep_status_t retval(WSREP_OK);
    const galera::MappedBuffer& wscoll(trx.write_set_collection());
    // skip over trx header
    size_t offset(galera::serial_size(trx));
    galera::WriteSet ws;
    while (offset < wscoll.size())
    {
        offset = unserialize(&wscoll[0], wscoll.size(), offset, ws);
        if ((retval = apply_ws(recv_ctx, apply_cb,
                               ws, trx.global_seqno())) != WSREP_OK)
        {
            break;
        }
    }
    assert(offset == wscoll.size() || retval != WSREP_OK);
    return retval;
}



static wsrep_status_t apply_trx_ws(void* recv_ctx,
                                   wsrep_bf_apply_cb_t apply_cb,
                                   const galera::TrxHandle& trx)
{
    static const size_t max_apply_attempts(10);
    size_t attempts(0);
    wsrep_status_t retval(WSREP_OK);
    do
    {
        retval = apply_wscoll(recv_ctx, apply_cb, trx);
        if (retval != WSREP_OK)
        {
            retval = apply_statement(recv_ctx,
                                     apply_cb,
                                     "rollback\0", 9,
                                     trx.global_seqno());
            if (retval != WSREP_OK)
            {
                attempts = max_apply_attempts;
                retval = WSREP_FATAL;
            }
            else
            {
                ++attempts;
            }
        }
    }
    while (retval != WSREP_OK && attempts < max_apply_attempts);

    if (attempts == max_apply_attempts)
    {
        assert(0);
        retval = WSREP_TRX_FAIL;
    }
    else
    {
        retval = apply_statement(recv_ctx, apply_cb,
                                 "commit\0", 7, trx.global_seqno());
        if (retval != WSREP_OK)
        {
            assert(0);
            retval = WSREP_FATAL;
        }
    }
    return retval;
}

std::ostream& galera::operator<<(std::ostream& os, ReplicatorSMM::State state)
{
    switch (state)
    {
    case ReplicatorSMM::S_CLOSED: return (os << "CLOSED");
    case ReplicatorSMM::S_CLOSING: return (os << "CLOSING");
    case ReplicatorSMM::S_JOINING: return (os << "JOINING");
    case ReplicatorSMM::S_JOINED: return (os << "JOINED");
    case ReplicatorSMM::S_SYNCED: return (os << "SYNCED");
    case ReplicatorSMM::S_DONOR: return (os << "DONOR");
    }
    gu_throw_fatal << "invalid state " << static_cast<int>(state);
    throw;
}


//////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////
//                           Public
//////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////


galera::ReplicatorSMM::ReplicatorSMM(const struct wsrep_init_args* args)
    :
    state_(S_CLOSED),
    sst_state_(SST_NONE),
    data_dir_(),
    state_file_("grastate.dat"),
    uuid_(WSREP_UUID_UNDEFINED),
    state_uuid_(WSREP_UUID_UNDEFINED),
    app_ctx_(args->app_ctx),
    logger_cb_(args->logger_cb),
    view_cb_(args->view_handler_cb),
    bf_apply_cb_(args->bf_apply_cb),
    sst_donate_cb_(args->sst_donate_cb),
    synced_cb_(args->synced_cb),
    sst_donor_(),
    sst_uuid_(WSREP_UUID_UNDEFINED),
    sst_seqno_(WSREP_SEQNO_UNDEFINED),
    sst_mutex_(),
    sst_cond_(),
    sst_retry_sec_(1),
    gcs_(args->node_name, args->node_incoming),
    service_thd_(gcs_),
    wsdb_(),
    cert_(),
    local_monitor_(),
    apply_monitor_(),
    receivers_(),
    replicated_(),
    replicated_bytes_(),
    received_(),
    received_bytes_(),
    local_commits_(),
    local_rollbacks_(),
    local_cert_failures_(),
    local_bf_aborts_(),
    local_replays_(),
    report_interval_(32),
    report_counter_(),
    wsrep_status_()
{
    // @todo add guards (and perhaps actions)
    state_.add_transition(Transition(S_CLOSED, S_JOINING));

    state_.add_transition(Transition(S_CLOSING, S_CLOSED));

    state_.add_transition(Transition(S_JOINING, S_CLOSING));
    state_.add_transition(Transition(S_JOINING, S_JOINED));
    state_.add_transition(Transition(S_JOINING, S_SYNCED));

    state_.add_transition(Transition(S_JOINED, S_CLOSING));
    state_.add_transition(Transition(S_JOINED, S_SYNCED));

    state_.add_transition(Transition(S_SYNCED, S_CLOSING));
    state_.add_transition(Transition(S_SYNCED, S_JOINING));
    state_.add_transition(Transition(S_SYNCED, S_DONOR));

    state_.add_transition(Transition(S_DONOR, S_JOINING));
    state_.add_transition(Transition(S_DONOR, S_JOINED));
    state_.add_transition(Transition(S_DONOR, S_SYNCED));
    state_.add_transition(Transition(S_DONOR, S_CLOSING));

    gu_conf_set_log_callback(reinterpret_cast<gu_log_cb_t>(args->logger_cb));
    local_monitor_.set_initial_position(0);
}

galera::ReplicatorSMM::~ReplicatorSMM()
{
    switch (state_())
    {
    case S_JOINING:
    case S_JOINED:
    case S_SYNCED:
    case S_DONOR:
        close();
    case S_CLOSING:
        // @todo wait that all users have left the building
    case S_CLOSED:
        break;
    }
}


wsrep_status_t galera::ReplicatorSMM::connect(const std::string& cluster_name,
                                   const std::string& cluster_url,
                                   const std::string& state_donor)
{
    state_.shift_to(S_JOINING);
    restore_state(state_file_);
    sst_donor_ = state_donor;
    service_thd_.reset();
    gcs_.set_initial_position(state_uuid_, cert_.position());
    gcs_.connect(cluster_name, cluster_url);
    return WSREP_OK;
}


wsrep_status_t galera::ReplicatorSMM::close()
{
    assert(state_() != S_CLOSED);
    gcs_.close();
    return WSREP_OK;
}



wsrep_status_t galera::ReplicatorSMM::async_recv(void* recv_ctx)
{
    assert(recv_ctx != 0);
    if (state_() == S_CLOSED || state_() == S_CLOSING)
    {
        log_error << "async recv cannot start, provider in closed/closing state";
        return WSREP_FATAL;
    }

    wsrep_status_t retval(WSREP_OK);
    while (state_() != S_CLOSING)
    {
        void* act;
        size_t act_size;
        gcs_act_type_t act_type;
        gcs_seqno_t seqno_g, seqno_l;
        ssize_t rc(gcs_.recv(&act, &act_size, &act_type, &seqno_l, &seqno_g));
        if (rc <= 0)
        {
            retval = WSREP_CONN_FAIL;
            break;
        }
        retval = dispatch(recv_ctx, act, act_size, act_type,
                          seqno_l, seqno_g);
        free(act);
        if (retval == WSREP_FATAL || retval == WSREP_NODE_FAIL) break;
    }

    if (receivers_.sub_and_fetch(1) == 0)
    {
        state_.shift_to(S_CLOSED);
    }
    return retval;
}

galera::TrxHandle*
galera::ReplicatorSMM::local_trx(wsrep_trx_id_t trx_id)
{
    return wsdb_.get_trx(uuid_, trx_id, false);
}

galera::TrxHandle*
galera::ReplicatorSMM::local_trx(wsrep_trx_handle_t* handle, bool create)
{
    TrxHandle* trx;
    assert(handle != 0);
    if (handle->opaque != 0)
    {
        trx = reinterpret_cast<TrxHandle*>(handle->opaque);
        assert(wsdb_.get_trx(uuid_, handle->trx_id) == trx);
        assert(trx->trx_id() == handle->trx_id);
        trx->ref();
    }
    else
    {
        trx = wsdb_.get_trx(uuid_, handle->trx_id, create);
        handle->opaque = trx;
    }
    return trx;
}


void galera::ReplicatorSMM::unref_local_trx(TrxHandle* trx)
{
    wsdb_.unref_trx(trx);
}

void galera::ReplicatorSMM::discard_local_trx(wsrep_trx_id_t trx_id)
{
    wsdb_.discard_trx(trx_id);
}


galera::TrxHandle*
galera::ReplicatorSMM::local_conn_trx(wsrep_conn_id_t conn_id, bool create)
{
    return wsdb_.get_conn_query(uuid_, conn_id, create);
}


void galera::ReplicatorSMM::set_default_context(wsrep_conn_id_t conn_id,
                                     const void* ctx, size_t ctx_len)
{
    wsdb_.set_conn_database(conn_id, ctx, ctx_len);
}


void galera::ReplicatorSMM::discard_local_conn(wsrep_conn_id_t conn_id)
{
    wsdb_.discard_conn(conn_id);
}


wsrep_status_t galera::ReplicatorSMM::process_trx_ws(void* recv_ctx,
                                          TrxHandle* trx)
{
    assert(trx != 0);
    assert(trx->global_seqno() > 0);
    assert(trx->is_local() == false);

    LocalOrder lo(*trx);
    ApplyOrder ao(*trx);

    local_monitor_.enter(lo);
    Certification::TestResult cert_ret(cert_.append_trx(trx));
    local_monitor_.leave(lo);

    wsrep_status_t retval(WSREP_OK);
    if (trx->global_seqno() > apply_monitor_.last_left())
    {
        switch (cert_ret)
        {
        case Certification::TEST_OK:
            apply_monitor_.enter(ao);
            retval = apply_trx_ws(recv_ctx, bf_apply_cb_, *trx);
            apply_monitor_.leave(ao);
            if (retval != WSREP_OK)
            {
                log_warn << "failed to apply trx " << *trx;
            }
            break;
        case Certification::TEST_FAILED:
            apply_monitor_.self_cancel(ao);
            retval = WSREP_TRX_FAIL;
            break;
        }
    }
    else
    {
        // This action was already contained in SST. Note that we can't
        // drop the action earlier to build cert index properly.
        log_debug << "skipping applying of trx " << *trx;
    }
    cert_.set_trx_committed(trx);
    report_last_committed();

    return retval;
}


wsrep_status_t galera::ReplicatorSMM::process_conn_ws(void* recv_ctx, TrxHandle* trx)
{
    assert(trx != 0);
    assert(trx->global_seqno() > 0);
    assert(trx->is_local() == false);

    LocalOrder lo(*trx);
    ApplyOrder ao(*trx);

    local_monitor_.enter(lo);

    Certification::TestResult cert_ret(cert_.append_trx(trx));

    wsrep_status_t retval(WSREP_OK);
    if (trx->global_seqno() > apply_monitor_.last_left())
    {
        switch (cert_ret)
        {
        case Certification::TEST_OK:
            apply_monitor_.drain(trx->global_seqno() - 1);
            retval = apply_wscoll(recv_ctx, bf_apply_cb_, *trx);
            break;
        case Certification::TEST_FAILED:
            retval = WSREP_TRX_FAIL;
            break;
        }
        apply_monitor_.self_cancel(ao);
    }
    else
    {
        // This action was already contained in SST. Note that we can't
        // drop the action earlier to build cert index properly.
        log_debug << "skipping applying of iso trx " << *trx;

    }
    cert_.set_trx_committed(trx);
    local_monitor_.leave(lo);

    return retval;
}


wsrep_status_t galera::ReplicatorSMM::replicate(TrxHandle* trx)
{
    if (state_() < S_JOINED) return WSREP_TRX_FAIL;

    assert(trx->state() == TrxHandle::S_EXECUTING ||
           trx->state() == TrxHandle::S_MUST_ABORT);
    assert(trx->local_seqno() == WSREP_SEQNO_UNDEFINED &&
           trx->global_seqno() == WSREP_SEQNO_UNDEFINED);


    if (trx->state() == TrxHandle::S_MUST_ABORT)
    {
        trx->set_state(TrxHandle::S_ABORTING);
        return WSREP_TRX_FAIL;
    }

    trx->set_state(TrxHandle::S_REPLICATING);

    gcs_seqno_t seqno_l(GCS_SEQNO_ILL), seqno_g(GCS_SEQNO_ILL);
    const MappedBuffer& wscoll(trx->write_set_collection());

    ssize_t rcode;
    do
    {
        assert(seqno_g == GCS_SEQNO_ILL);

        const ssize_t gcs_handle(gcs_.schedule());
        if (gcs_handle < 0)
        {
            log_debug << "gcs schedule " << strerror(-gcs_handle);
            trx->set_state(TrxHandle::S_ABORTING);
            return WSREP_TRX_FAIL;
        }
        trx->set_gcs_handle(gcs_handle);
        trx->set_last_seen_seqno(apply_monitor_.last_left());
        trx->flush(0);

        trx->unlock();
        rcode = gcs_.repl(&wscoll[0], wscoll.size(),
                          GCS_ACT_TORDERED, true, &seqno_l, &seqno_g);
        trx->lock();
    }
    while (rcode == -EAGAIN && trx->state() != TrxHandle::S_MUST_ABORT &&
           (usleep(1000), true));

    if (rcode < 0)
    {
        if (rcode != -EINTR)
        {
            log_debug << "gcs_repl() failed with " << strerror(-rcode)
                      << " for trx " << *trx;
        }
        assert(rcode != -EINTR || trx->state() == TrxHandle::S_MUST_ABORT);
        assert(seqno_l == GCS_SEQNO_ILL && seqno_g == GCS_SEQNO_ILL);
        trx->set_state(TrxHandle::S_ABORTING);
        trx->set_gcs_handle(-1);
        return WSREP_TRX_FAIL;
    }

    assert(seqno_l != GCS_SEQNO_ILL && seqno_g != GCS_SEQNO_ILL);
    trx->set_gcs_handle(-1);
    trx->set_seqnos(seqno_l, seqno_g);

    wsrep_status_t retval;
    if (trx->state() == TrxHandle::S_MUST_ABORT)
    {
        retval = cert_for_aborted(trx);
        if (retval != WSREP_BF_ABORT)
        {
            LocalOrder lo(*trx);
            ApplyOrder ao(*trx);
            local_monitor_.self_cancel(lo);
            apply_monitor_.self_cancel(ao);
        }
    }
    else
    {
        trx->set_state(TrxHandle::S_REPLICATED);
        ++replicated_;
        replicated_bytes_ += wscoll.size();
        retval = WSREP_OK;
    }

    return retval;
}


wsrep_status_t galera::ReplicatorSMM::abort(TrxHandle* trx)
{
    assert(trx != 0);
    assert(trx->is_local() == true);

    log_debug << "aborting trx " << *trx << " " << trx;

    wsrep_status_t retval(WSREP_OK);
    switch (trx->state())
    {
    case TrxHandle::S_MUST_ABORT:
    case TrxHandle::S_ABORTING:
        // nothing to do
        break;
    case TrxHandle::S_EXECUTING:
        trx->set_state(TrxHandle::S_MUST_ABORT);
        break;
    case TrxHandle::S_REPLICATING:
    {
        // trx is in gcs repl
        trx->set_state(TrxHandle::S_MUST_ABORT);
        int rc;
        if (trx->gcs_handle() > 0 &&
            ((rc = gcs_.interrupt(trx->gcs_handle()))) != 0)
        {
            log_debug << "gcs_interrupt(): handle "
                      << trx->gcs_handle()
                      << " trx id " << trx->trx_id()
                      << ": " << strerror(-rc);
        }
        break;
    }
    case TrxHandle::S_CERTIFYING:
    {
        // trx is waiting in local monitor
        trx->set_state(TrxHandle::S_MUST_ABORT);
        LocalOrder lo(*trx);
        trx->unlock();
        local_monitor_.interrupt(lo);
        trx->lock();
        break;
    }
    case TrxHandle::S_CERTIFIED:
    {
        // trx is waiting in apply monitor
        trx->set_state(TrxHandle::S_MUST_ABORT);
        ApplyOrder ao(*trx);
        trx->unlock();
        apply_monitor_.interrupt(ao);
        trx->lock();
        break;
    }
    default:
        gu_throw_fatal << "invalid state " << trx->state();
        throw;
    }

    ++local_bf_aborts_;

    return retval;
}


wsrep_status_t galera::ReplicatorSMM::pre_commit(TrxHandle* trx)
{
    if (state_() < S_JOINED) return WSREP_TRX_FAIL;

    assert(trx->state() == TrxHandle::S_REPLICATED);
    assert(trx->local_seqno() > -1 && trx->global_seqno() > -1);

    wsrep_status_t retval(cert(trx));
    if (retval != WSREP_OK)
    {
        assert(trx->state() == TrxHandle::S_ABORTING ||
               trx->state() == TrxHandle::S_MUST_CERT_AND_REPLAY);
        return retval;
    }
    assert(trx->state() == TrxHandle::S_CERTIFIED);
    assert(trx->global_seqno() > apply_monitor_.last_left());

    ApplyOrder ao(*trx);
    int rc(apply_monitor_.enter(ao));
    assert(rc == 0 || rc == -EINTR);

    if (rc == -EINTR)
    {
        assert(trx->state() == TrxHandle::S_MUST_ABORT);
        if (cert_for_aborted(trx) == WSREP_OK)
        {
            assert(trx->state() == TrxHandle::S_MUST_REPLAY);
            retval = WSREP_BF_ABORT;
        }
        else
        {
            apply_monitor_.self_cancel(ao);
            trx->set_state(TrxHandle::S_ABORTING);
            retval = WSREP_TRX_FAIL;
        }
    }
    else if ((trx->flags() & TrxHandle::F_COMMIT) != 0)
    {
        trx->set_state(TrxHandle::S_APPLYING);
    }
    else
    {
        trx->set_state(TrxHandle::S_EXECUTING);
    }

    assert((retval == WSREP_OK && (trx->state() == TrxHandle::S_APPLYING ||
                                   trx->state() == TrxHandle::S_EXECUTING))
           ||
           (retval == WSREP_TRX_FAIL && trx->state() == TrxHandle::S_ABORTING)
           ||
           (retval == WSREP_BF_ABORT &&
            trx->state() == TrxHandle::S_MUST_REPLAY));

    return retval;
}

wsrep_status_t galera::ReplicatorSMM::replay(TrxHandle* trx, void* trx_ctx)
{
    assert(trx->state() == TrxHandle::S_MUST_CERT_AND_REPLAY ||
           trx->state() == TrxHandle::S_MUST_REPLAY);
    assert(trx->trx_id() != static_cast<wsrep_trx_id_t>(-1));
    assert(trx->global_seqno() > apply_monitor_.last_left());

    wsrep_status_t retval(WSREP_OK);
    switch (trx->state())
    {
    case TrxHandle::S_MUST_CERT_AND_REPLAY:
        retval = cert(trx);
        if (retval != WSREP_OK)
        {
            ApplyOrder ao(*trx);
            apply_monitor_.self_cancel(ao);
            break;
        }
        // fall through
    case TrxHandle::S_MUST_REPLAY:
    {
        // safety measure to make sure that all preceding trxs finish before
        // replaying
        trx->set_last_depends_seqno(trx->global_seqno() - 1);
        trx->set_state(TrxHandle::S_REPLAYING);
        ApplyOrder ao(*trx);
        apply_monitor_.enter(ao);
        retval = apply_trx_ws(trx_ctx, bf_apply_cb_, *trx);
        ++local_replays_;
        // apply monitor is released in post commit
        // apply_monitor_.leave(ao);
        break;
    }
    default:
        gu_throw_fatal << "invalid state in replay for trx " << *trx;
    }

    if (retval != WSREP_OK)
    {
        log_debug << "replaying failed for trx " << trx;
        trx->set_state(TrxHandle::S_ABORTING);
    }
    else
    {
        log_debug << "replaying successfull for trx " << trx;
        trx->set_state(TrxHandle::S_REPLAYED);
    }
    return retval;
}


wsrep_status_t galera::ReplicatorSMM::post_commit(TrxHandle* trx)
{
    assert(trx->state() == TrxHandle::S_APPLYING ||
           trx->state() == TrxHandle::S_REPLAYED);
    assert(trx->local_seqno() > -1 && trx->global_seqno() > -1);

    ApplyOrder ao(*trx);
    apply_monitor_.leave(ao);
    cert_.set_trx_committed(trx);
    report_last_committed();
    ++local_commits_;
    return WSREP_OK;
}


wsrep_status_t galera::ReplicatorSMM::post_rollback(TrxHandle* trx)
{
    assert(trx->state() == TrxHandle::S_ABORTING ||
           trx->state() == TrxHandle::S_EXECUTING);

    trx->set_state(TrxHandle::S_ROLLED_BACK);
    report_last_committed();
    ++local_rollbacks_;

    return WSREP_OK;
}


wsrep_status_t galera::ReplicatorSMM::causal_read(wsrep_seqno_t* seqno) const
{
    return WSREP_NOT_IMPLEMENTED;
}


wsrep_status_t galera::ReplicatorSMM::to_isolation_begin(TrxHandle* trx)
{
    assert(trx->state() == TrxHandle::S_REPLICATED);
    assert(trx->trx_id() == static_cast<wsrep_trx_id_t>(-1));
    assert(trx->local_seqno() > -1 && trx->global_seqno() > -1);
    assert(trx->global_seqno() > apply_monitor_.last_left());

    trx->set_state(TrxHandle::S_CERTIFYING);
    LocalOrder lo(*trx);
    if (local_monitor_.enter(lo) != 0)
    {
        local_monitor_.self_cancel(lo);
        ApplyOrder ao(*trx);
        apply_monitor_.self_cancel(ao);
        trx->set_state(TrxHandle::S_ABORTING);
        return WSREP_TRX_FAIL;
    }

    const Certification::TestResult cert_ret(cert_.append_trx(trx));
    wsrep_status_t retval(WSREP_OK);
    switch (cert_ret)
    {
    case Certification::TEST_OK:
        trx->set_state(TrxHandle::S_CERTIFIED);
        apply_monitor_.drain(trx->global_seqno() - 1);
        trx->set_state(TrxHandle::S_APPLYING);
        retval = WSREP_OK;
        break;

    case Certification::TEST_FAILED:
    {
        assert(trx->state() == TrxHandle::S_ABORTING);
        local_monitor_.leave(lo);
        ApplyOrder ao(*trx);
        apply_monitor_.self_cancel(ao);
        cert_.set_trx_committed(trx);
        retval = WSREP_TRX_FAIL;
        break;
    }
    }

    return retval;
}


wsrep_status_t galera::ReplicatorSMM::to_isolation_end(TrxHandle* trx)
{
    assert(trx->state() == TrxHandle::S_APPLYING);

    LocalOrder lo(*trx);
    local_monitor_.leave(lo);
    ApplyOrder ao(*trx);
    apply_monitor_.self_cancel(ao);
    trx->set_state(TrxHandle::S_COMMITTED);
    cert_.set_trx_committed(trx);
    wsdb_.discard_conn_query(trx->conn_id());
    report_last_committed();

    return WSREP_OK;
}


wsrep_status_t
galera::ReplicatorSMM::sst_sent(const wsrep_uuid_t& uuid, wsrep_seqno_t seqno)
{
    if (state_() != S_DONOR)
    {
        log_error << "sst sent called when not SST donor, state " << state_();
        return WSREP_CONN_FAIL;
    }

    if (uuid != state_uuid_ && seqno >= 0)
    {
        // state we have sent no longer corresponds to the current group state
        // mark an error
        seqno = -EREMCHG;
    }

    // WARNING: Here we have application block on this call which
    //          may prevent application from resolving the issue.
    //          (Not that we expect that application can resolve it.)
    ssize_t err;
    while (-EAGAIN == (err = gcs_.join(seqno))) usleep (100000);

    if (err == 0) return WSREP_OK;

    log_error << "failed to recover from DONOR state";

    return WSREP_CONN_FAIL;
}


wsrep_status_t
galera::ReplicatorSMM::sst_received(const wsrep_uuid_t& uuid,
                         wsrep_seqno_t seqno,
                         const void* state,
                         size_t state_len)
{
    if (state_() != S_JOINING)
    {
        log_error << "not in joining state when sst received called, state "
                  << state_();
        return WSREP_CONN_FAIL;
    }
    gu::Lock lock(sst_mutex_);
    sst_uuid_ = uuid;
    sst_seqno_ = seqno;
    sst_cond_.signal();
    return WSREP_OK;
}


void galera::ReplicatorSMM::store_state(const std::string& file) const
{
    std::ofstream fs(file.c_str(), std::ios::trunc);
    if (fs.fail() == true)
    {
        gu_throw_fatal << "could not store state";
    }

    fs << "# GALERA saved state, version: " << 0.7 << ", date: (todo)\n";
    fs << "uuid:  " << state_uuid_ << "\n";
    fs << "seqno: " << apply_monitor_.last_left() << "\n";
    fs << "cert_index:\n";
}

void galera::ReplicatorSMM::restore_state(const std::string& file)
{
    std::ifstream fs(file.c_str());
    if (fs.fail() == true)
    {
        log_warn << "could not restore state from file " << file;
        return;
    }

    std::string line;
    std::string param;
    wsrep_uuid_t uuid(WSREP_UUID_UNDEFINED);
    wsrep_seqno_t seqno(WSREP_SEQNO_UNDEFINED);

    getline(fs, line);
    if (fs.good() == false)
    {
        gu_throw_fatal << "could not read header from file " << file;
    }
    log_debug << "read state header: "<< line;

    while (fs.good() == true)
    {
        getline(fs, line);
        if (fs.good() == false) break;

        std::istringstream istr(line);
        istr >> param;
        if (param == "uuid:")
        {
            istr >> uuid;
            log_debug << "read state uuid " << uuid;
        }
        else if (param == "seqno:")
        {
            istr >> seqno;
            log_debug << "read seqno " << seqno;
        }
        else if (param == "cert_index:")
        {
            // @todo
            log_debug << "cert index restore not implemented yet";
        }
    }

    state_uuid_ = uuid;
    apply_monitor_.set_initial_position(seqno);
    cert_.assign_initial_position(seqno);
}


void galera::ReplicatorSMM::invalidate_state(const std::string& file) const
{
    std::ofstream fs(file.c_str(), std::ios::trunc);
    if (fs.fail() == true)
    {
        gu_throw_fatal << "could not store state";
    }

    fs << "# GALERA saved state, version: " << 0.7 << ", date: (todo)\n";
    fs << "uuid:  " << WSREP_UUID_UNDEFINED << "\n";
    fs << "seqno: " << WSREP_SEQNO_UNDEFINED << "\n";
    fs << "cert_index:\n";
}

static const size_t GALERA_STAGE_MAX(10);
static const char* status_str[GALERA_STAGE_MAX] =
{
    "Initialized (0)",
    "Joining (1)",
    "Prepare for SST (2)",
    "SST request sent (3)",
    "Waiting for SST (4)",
    "Joined (5)",
    "Synced (6)",
    "Donor (+)"
    "SST request failed (-)",
    "SST failed (-)",
};

static wsrep_member_status_t state2status(galera::ReplicatorSMM::State state)
{
//    using galera::ReplicatorSMM;
    switch (state)
    {
    case galera::ReplicatorSMM::S_CLOSED  : return WSREP_MEMBER_EMPTY;
    case galera::ReplicatorSMM::S_CLOSING : return WSREP_MEMBER_EMPTY;
    case galera::ReplicatorSMM::S_JOINING : return WSREP_MEMBER_JOINER;
    case galera::ReplicatorSMM::S_JOINED  : return WSREP_MEMBER_JOINED;
    case galera::ReplicatorSMM::S_SYNCED  : return WSREP_MEMBER_SYNCED;
    case galera::ReplicatorSMM::S_DONOR   : return WSREP_MEMBER_DONOR;
    }
    gu_throw_fatal << "invalid state " << state;
    throw;
}

static const char* state2status_str(galera::ReplicatorSMM::State state,
                                    galera::ReplicatorSMM::SstState sst_state)
{
    using galera::ReplicatorSMM;
    switch (state)
    {
    case galera::ReplicatorSMM::S_CLOSED :
    case galera::ReplicatorSMM::S_CLOSING:
    {
        if (sst_state == ReplicatorSMM::SST_REQ_FAILED)  return status_str[8];
        else if (sst_state == ReplicatorSMM::SST_FAILED) return status_str[9];
        else                                             return status_str[0];
    }
    case galera::ReplicatorSMM::S_JOINING:
    {
        if (sst_state == ReplicatorSMM::SST_WAIT) return status_str[4];
        else                                      return status_str[1];
    }
    case galera::ReplicatorSMM::S_JOINED : return status_str[5];
    case galera::ReplicatorSMM::S_SYNCED : return status_str[6];
    case galera::ReplicatorSMM::S_DONOR  : return status_str[7];
    }
    gu_throw_fatal << "invalid state " << state;
    throw;
}

typedef enum status_vars
{
    STATUS_STATE_UUID = 0,
    STATUS_LAST_APPLIED,
    STATUS_REPLICATED,
    STATUS_REPLICATED_BYTES,
    STATUS_RECEIVED,
    STATUS_RECEIVED_BYTES,
    STATUS_LOCAL_COMMITS,
    STATUS_LOCAL_CERT_FAILURES,
    STATUS_LOCAL_BF_ABORTS,
    STATUS_LOCAL_REPLAYS,
    STATUS_LOCAL_SLAVE_QUEUE,
    STATUS_FC_WAITS,
    STATUS_CERT_DEPS_DISTANCE,
    STATUS_APPLY_OOOE,
    STATUS_APPLY_OOOL,
    STATUS_APPLY_WINDOW,
    STATUS_LOCAL_STATUS,
    STATUS_LOCAL_STATUS_COMMENT,
    STATUS_MAX
} StatusVars;

static struct wsrep_status_var wsrep_status[STATUS_MAX + 1] =
{
    {"local_state_uuid",    WSREP_STATUS_STRING, { 0 }                      },
    {"last_committed",      WSREP_STATUS_INT64,  { -1 }                     },
    {"replicated",          WSREP_STATUS_INT64,  { 0 }                      },
    {"replicated_bytes",    WSREP_STATUS_INT64,  { 0 }                      },
    {"received",            WSREP_STATUS_INT64,  { 0 }                      },
    {"received_bytes",      WSREP_STATUS_INT64,  { 0 }                      },
    {"local_commits",       WSREP_STATUS_INT64,  { 0 }                      },
    {"local_cert_failures", WSREP_STATUS_INT64,  { 0 }                      },
    {"local_bf_aborts",     WSREP_STATUS_INT64,  { 0 }                      },
    {"local_replays",       WSREP_STATUS_INT64,  { 0 }                      },
    {"local_slave_queue",   WSREP_STATUS_INT64,  { 0 }                      },
    {"flow_control_waits",  WSREP_STATUS_INT64,  { 0 }                      },
    {"cert_deps_distance",  WSREP_STATUS_DOUBLE, { 0 }                      },
    {"apply_oooe",          WSREP_STATUS_DOUBLE, { 0 }                      },
    {"apply_oool",          WSREP_STATUS_DOUBLE, { 0 }                      },
    {"apply_window",        WSREP_STATUS_DOUBLE, { 0 }                      },
    {"local_status",        WSREP_STATUS_INT64,  { 0 }                      },
    {"local_status_comment",WSREP_STATUS_STRING, { 0 }                      },
    {0, WSREP_STATUS_STRING, { 0 }}
};


static void build_status_vars(std::vector<struct wsrep_status_var>& status)
{
    struct wsrep_status_var* ptr(wsrep_status);
    do
    {
        status.push_back(*ptr);
    }
    while (ptr++->name != 0);
}

const struct wsrep_status_var* galera::ReplicatorSMM::status() const
{
    std::vector<struct wsrep_status_var>&
        sv(const_cast<std::vector<struct wsrep_status_var>& >(wsrep_status_));
    if (sv.empty() == true)
    {
        build_status_vars(sv);;
    }

    free(const_cast<char*>(sv[STATUS_STATE_UUID].value._string));
    std::ostringstream os;
    os << state_uuid_;
    sv[STATUS_STATE_UUID].value._string = strdup(os.str().c_str());
    sv[STATUS_LAST_APPLIED       ].value._int64 =
        apply_monitor_.last_left();;
    sv[STATUS_REPLICATED         ].value._int64 = replicated_();
    sv[STATUS_REPLICATED_BYTES   ].value._int64 =
        replicated_bytes_();
    sv[STATUS_RECEIVED           ].value._int64 = received_();
    sv[STATUS_RECEIVED_BYTES     ].value._int64 = received_bytes_();
    sv[STATUS_LOCAL_COMMITS      ].value._int64 = local_commits_();
    sv[STATUS_LOCAL_CERT_FAILURES].value._int64 =
        local_cert_failures_();
    sv[STATUS_LOCAL_BF_ABORTS    ].value._int64 = local_bf_aborts_();
    sv[STATUS_LOCAL_REPLAYS      ].value._int64 = local_replays_();
    sv[STATUS_LOCAL_SLAVE_QUEUE  ].value._int64 =
        gcs_.queue_len();
    sv[STATUS_FC_WAITS           ].value._int64 = 0;
    sv[STATUS_CERT_DEPS_DISTANCE ].value._double =
        cert_.get_avg_deps_dist();
    double oooe;
    double oool;
    double win;
    const_cast<Monitor<ApplyOrder>&>(apply_monitor_).get_stats(&oooe, &oool, &win);
    sv[STATUS_APPLY_OOOE         ].value._double = oooe;
    sv[STATUS_APPLY_OOOL         ].value._double = oool;
    sv[STATUS_APPLY_WINDOW       ].value._double = win;
    sv[STATUS_LOCAL_STATUS       ].value._int64 =  state2status(state_());
    sv[STATUS_LOCAL_STATUS_COMMENT].value._string = state2status_str(state_(), sst_state_);

    return &wsrep_status_[0];
}


//////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////
//                           Private
//////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////


wsrep_status_t galera::ReplicatorSMM::cert(TrxHandle* trx)
{
    assert(trx->state() == TrxHandle::S_REPLICATED ||
           trx->state() == TrxHandle::S_MUST_CERT_AND_REPLAY);
    assert(trx->local_seqno() != WSREP_SEQNO_UNDEFINED &&
           trx->global_seqno() != WSREP_SEQNO_UNDEFINED &&
           trx->last_seen_seqno() != WSREP_SEQNO_UNDEFINED);

    trx->set_state(TrxHandle::S_CERTIFYING);

    LocalOrder lo(*trx);
    ApplyOrder ao(*trx);

    const int rcode(local_monitor_.enter(lo));
    assert(rcode == 0 || rcode == -EINTR);

    wsrep_status_t retval(WSREP_OK);
    if (rcode == -EINTR)
    {
        retval = cert_for_aborted(trx);
        if (retval != WSREP_BF_ABORT)
        {
            local_monitor_.self_cancel(lo);
            apply_monitor_.self_cancel(ao);
        }
    }
    else
    {
        switch (cert_.append_trx(trx))
        {
        case Certification::TEST_OK:
            trx->set_state(TrxHandle::S_CERTIFIED);
            retval = WSREP_OK;
            break;
        case Certification::TEST_FAILED:
            apply_monitor_.self_cancel(ao);
            trx->set_state(TrxHandle::S_ABORTING);
            ++local_cert_failures_;
            cert_.set_trx_committed(trx);
            retval = WSREP_TRX_FAIL;
            break;
        }
        local_monitor_.leave(lo);
    }

    log_debug << "cert for " << *trx << " " << retval;
    return retval;
}


wsrep_status_t galera::ReplicatorSMM::cert_for_aborted(TrxHandle* trx)
{
    wsrep_status_t retval(WSREP_OK);
    switch (cert_.test(trx, false))
    {
    case Certification::TEST_OK:
        trx->set_state(TrxHandle::S_MUST_CERT_AND_REPLAY);
        retval = WSREP_BF_ABORT;
        break;
    case Certification::TEST_FAILED:
        trx->set_state(TrxHandle::S_ABORTING);
        retval = WSREP_TRX_FAIL;
        break;
    }
    return retval;
}

void galera::ReplicatorSMM::report_last_committed()
{
    size_t i(report_counter_.fetch_and_add(1));
    if (i % report_interval_ == 0)
        service_thd_.report_last_committed(apply_monitor_.last_left());
}


wsrep_status_t galera::ReplicatorSMM::process_global_action(void* recv_ctx,
                                                 const void* act,
                                                 size_t act_size,
                                                 wsrep_seqno_t seqno_l,
                                                 wsrep_seqno_t seqno_g)
{
    assert(recv_ctx != 0);
    assert(act != 0);
    assert(seqno_l > 0);
    assert(seqno_g > 0);

    if (seqno_g <= cert_.position())
    {
        log_debug << "global trx below cert position" << seqno_g;
        return WSREP_OK;
    }

    TrxHandle* trx(cert_.create_trx(act, act_size, seqno_l, seqno_g));
    if (trx == 0)
    {
        log_warn << "could not read trx " << seqno_g;
        return WSREP_FATAL;
    }
    wsrep_status_t retval;
    TrxHandleLock lock(*trx);
    if (trx->trx_id() != static_cast<wsrep_trx_id_t>(-1))
    {
        // normal trx
        retval = process_trx_ws(recv_ctx, trx);
    }
    else
    {
        // trx to be run in isolation
        retval = process_conn_ws(recv_ctx, trx);
    }
    trx->unref();

    return retval;
}



wsrep_status_t galera::ReplicatorSMM::request_sst(wsrep_uuid_t const& group_uuid,
                                       wsrep_seqno_t const group_seqno,
                                       const void* req, size_t req_len)
{
    assert(req != 0);
    log_info << "State transfer required: "
             << "\n\tGroup state: "
             << group_uuid << ":" << group_seqno
             << "\n\tLocal state: " << state_uuid_
             << ":" << apply_monitor_.last_left();

    wsrep_status_t retval(WSREP_OK);
    gu::Lock lock(sst_mutex_);
    long ret;
    do
    {
        invalidate_state(state_file_);
        gcs_seqno_t seqno_l;
        ret = gcs_.request_state_transfer(req, req_len, sst_donor_, &seqno_l);
        if (ret < 0)
        {
            if (ret != -EAGAIN)
            {
                store_state(state_file_);
                log_error << "Requesting state snapshot transfer failed: "
                          << ret << "(" << strerror(-ret) << ")";
            }
            else
            {
                log_info << "Requesting state snapshot transfer failed: "
                         << ret << "(" << strerror(-ret) << "). "
                         << "Retrying in " << sst_retry_sec_ << " seconds";
            }
        }
        if (seqno_l != GCS_SEQNO_ILL)
        {
            // we are already holding local monitor
            LocalOrder lo(seqno_l);
            local_monitor_.self_cancel(lo);
        }
    }
    while ((ret == -EAGAIN) && (usleep(sst_retry_sec_ * 1000000), true));


    if (ret >= 0)
    {
        log_info << "Requesting state transfer: success, donor " << ret;
        sst_state_ = SST_WAIT;
        lock.wait(sst_cond_);
        if (sst_uuid_ != group_uuid || sst_seqno_ < group_seqno)
        {
            log_fatal << "Application received wrong state: "
                      << "\n\tReceived: "
                      << sst_uuid_ <<   ":    " << sst_seqno_
                      << "\n\tRequired: "
                      << group_uuid << ": >= " << group_seqno;
            sst_state_ = SST_FAILED;
            gu_throw_fatal << "Application state transfer failed";
        }
        else
        {
            state_uuid_ = sst_uuid_;
            apply_monitor_.set_initial_position(-1);
            apply_monitor_.set_initial_position(sst_seqno_);
            log_debug << "Initial state " << state_uuid_ << ":" << sst_seqno_;
            sst_state_ = SST_NONE;
            gcs_.join(sst_seqno_);
        }
    }
    else
    {
        sst_state_ = SST_REQ_FAILED;
        retval = WSREP_FATAL;
    }
    return retval;
}


bool galera::ReplicatorSMM::st_required(const gcs_act_conf_t& conf)
{
    bool retval(conf.my_state == GCS_NODE_STATE_PRIM);
    const wsrep_uuid_t* group_uuid(
        reinterpret_cast<const wsrep_uuid_t*>(conf.group_uuid));

    if (retval == true)
    {
        assert(conf.conf_id >= 0);
        if (state_uuid_ == *group_uuid)
        {
            // common history
            if (state_() >= S_JOINED)
            {
                // if we took ST already, it may exceed conf->seqno
                // (ST is asynchronous!)
                retval = (apply_monitor_.last_left() < conf.seqno);
            }
            else
            {
                // here we are supposed to have continuous history
                retval = (apply_monitor_.last_left() != conf.seqno);
            }
        }
        else
        {
            // no common history
        }
    }
    else
    {
        // non-prim component
        // assert(conf.conf_id < 0);
    }
    return retval;
}


wsrep_status_t galera::ReplicatorSMM::process_conf(void* recv_ctx,
                                        const gcs_act_conf_t* conf)
{
    assert(conf != 0);

    bool st_req(st_required(*conf));
    const wsrep_seqno_t group_seqno(conf->seqno);
    const wsrep_uuid_t* group_uuid(
        reinterpret_cast<const wsrep_uuid_t*>(conf->group_uuid));
    wsrep_view_info_t* view_info(galera_view_info_create(conf, st_req));

    uuid_ = view_info->members[view_info->my_idx].id;

    void* app_req(0);
    ssize_t app_req_len(0);
    view_cb_(app_ctx_, recv_ctx, view_info, 0, 0, &app_req, &app_req_len);

    wsrep_status_t retval(WSREP_OK);
    if (conf->conf_id >= 0)
    {
        // Primary configuration

        // we have to reset cert initial position here, SST does not contain
        // cert index yet (see #197).
        cert_.assign_initial_position(conf->seqno);

        if (st_req == true)
        {
            retval = request_sst(*group_uuid, group_seqno, app_req, app_req_len);
        }
        else
        {
            // sanity checks here
            if (conf->conf_id == 1)
            {
                state_uuid_ = *group_uuid;
                apply_monitor_.set_initial_position(conf->seqno);
            }

            if (state_() == S_JOINING || state_() == S_DONOR)
            {
                switch (conf->my_state)
                {
                case GCS_NODE_STATE_JOINED:
                    state_.shift_to(S_JOINED);
                    break;
                case GCS_NODE_STATE_SYNCED:
                    state_.shift_to(S_SYNCED);
                    synced_cb_(app_ctx_);
                    break;
                default:
                    log_debug << "gcs state " << conf->my_state;
                    break;
                }
            }
            invalidate_state(state_file_);
        }
    }
    else
    {
        // Non-primary configuration
        if (state_uuid_ != WSREP_UUID_UNDEFINED)
        {
            store_state(state_file_);
        }
        if (conf->my_idx >= 0)
        {
            state_.shift_to(S_JOINING);
        }
        else
        {
            state_.shift_to(S_CLOSING);
        }
    }
    return retval;
}


wsrep_status_t galera::ReplicatorSMM::process_to_action(void* recv_ctx,
                                             const void* act,
                                             size_t act_size,
                                             gcs_act_type_t act_type,
                                             wsrep_seqno_t seqno_l)
{
    assert(seqno_l > -1);
    LocalOrder lo(seqno_l);
    local_monitor_.enter(lo);
    apply_monitor_.drain(cert_.position());

    wsrep_status_t retval(WSREP_OK);
    switch (act_type)
    {
    case GCS_ACT_CONF:
        retval = process_conf(recv_ctx,
                              reinterpret_cast<const gcs_act_conf_t*>(act));
        break;

    case GCS_ACT_STATE_REQ:
        state_.shift_to(S_DONOR);
        sst_donate_cb_(app_ctx_, recv_ctx, act, act_size,
                       &state_uuid_, cert_.position(), 0, 0);
        retval = WSREP_OK;
        break;

    case GCS_ACT_JOIN:
        state_.shift_to(S_JOINED);
        retval = WSREP_OK;
        break;

    case GCS_ACT_SYNC:
        state_.shift_to(S_SYNCED);
        synced_cb_(app_ctx_);
        retval = WSREP_OK;
        break;

    default:
        log_fatal << "invalid gcs act type " << act_type;
        gu_throw_fatal << "invalid gcs act type " << act_type;
        throw;
    }
    local_monitor_.leave(lo);
    return retval;
}



wsrep_status_t galera::ReplicatorSMM::dispatch(void* recv_ctx,
                                    const void* act,
                                    size_t act_size,
                                    gcs_act_type_t act_type,
                                    wsrep_seqno_t seqno_l,
                                    wsrep_seqno_t seqno_g)
{
    assert(recv_ctx != 0);
    assert(act != 0);
    switch (act_type)
    {
    case GCS_ACT_TORDERED:
    {
        assert(seqno_l != GCS_SEQNO_ILL && seqno_g != GCS_SEQNO_ILL);
        ++received_;
        received_bytes_ += act_size;
        return process_global_action(recv_ctx, act, act_size, seqno_l, seqno_g);
    }

    case GCS_ACT_COMMIT_CUT:
    {
        assert(seqno_g == GCS_SEQNO_ILL);
        LocalOrder lo(seqno_l);
        local_monitor_.enter(lo);
        wsrep_seqno_t seq;
        unserialize(reinterpret_cast<const gu::byte_t*>(act), act_size, 0, seq);
        cert_.purge_trxs_upto(seq);
        local_monitor_.leave(lo);
        return WSREP_OK;
    }

    default:
    {
        // assert(seqno_g == GCS_SEQNO_ILL);
        if (seqno_l < 0)
        {
            log_error << "got error " << gcs_act_type_to_str(act_type);
            return WSREP_OK;
        }
        return process_to_action(recv_ctx, act, act_size, act_type, seqno_l);
    }
    }
}

