/*
 * (C) 2017 The University of Chicago
 * 
 * See COPYRIGHT in top-level directory.
 */

//#define FAKE_CPP_SERVER

#include <assert.h>
#include <mpi.h>
#include <abt.h>
#include <margo.h>
//#include <sds-keyval.h>
#include <bake-server.h>
#include <bake-client.h>
#include <ssg-mpi.h>

#include "mobject-server.h"
#include "src/server/mobject-server-context.h"
#include "src/rpc-types/write-op.h"
#include "src/rpc-types/read-op.h"
//#include "src/server/print-write-op.h"
//#include "src/server/print-read-op.h"
#include "src/io-chain/write-op-impl.h"
#include "src/io-chain/read-op-impl.h"
#include "src/server/visitor-args.h"
#ifdef FAKE_CPP_SERVER
#include "src/server/fake/fake-read-op.h"
#include "src/server/fake/fake-write-op.h"
#else
#include "src/server/core/core-read-op.h"
#include "src/server/core/core-write-op.h"
#endif

DECLARE_MARGO_RPC_HANDLER(mobject_write_op_ult)
DECLARE_MARGO_RPC_HANDLER(mobject_read_op_ult)
DECLARE_MARGO_RPC_HANDLER(mobject_shutdown_ult)

static void mobject_finalize_cb(void* data);

int mobject_provider_register(
        margo_instance_id mid,
        uint8_t mplex_id,
        ABT_pool pool,
        bake_provider_handle_t bake_ph,
        const char *cluster_file,
        mobject_provider_t* provider)
{
    mobject_provider_t srv_ctx;
    int my_id;
    int ret;

    /* check if a provider with the same multiplex id already exists */
    {
        hg_id_t id;
        hg_bool_t flag;
        margo_registered_name_mplex(mid, "mobject_write_op", mplex_id, &id, &flag);
        if(flag == HG_TRUE) {
            fprintf(stderr, "mobject_provider_register(): a provider with the same mplex id (%d) already exists\n", mplex_id);
            return -1;
        }
    }


    srv_ctx = calloc(1, sizeof(*srv_ctx));
    if (!srv_ctx)
        return -1;
    srv_ctx->mid = mid;
    srv_ctx->mplex_id = mplex_id;
    srv_ctx->pool = pool;
    srv_ctx->ref_count = 1;

    /* server group create */
    srv_ctx->gid =  ssg_group_create_mpi(MOBJECT_SERVER_GROUP_NAME, MPI_COMM_WORLD, NULL, NULL); 
        /* XXX membership update callbacks unused currently */
    if (srv_ctx->gid == SSG_GROUP_ID_NULL)
    {
        fprintf(stderr, "Error: Unable to create the mobject server group\n");
        free(srv_ctx);
        return -1;
    }
    my_id = ssg_get_group_self_id(srv_ctx->gid);

    /* one proccess writes cluster connect info to file for clients to find later */
    if (my_id == 0)
    {
        ret = ssg_group_id_store(cluster_file, srv_ctx->gid);
        if (ret != 0)
        {
            fprintf(stderr, "Error: unable to store mobject cluster info to file %s\n",
                cluster_file);
            /* XXX: this call is performed by one process, and we do not currently
             * have an easy way to propagate this error to the entire cluster group
             */
            ssg_group_destroy(srv_ctx->gid);
            free(srv_ctx);
            return -1;
        }
    }

    /* Bake settings initialization */
    bake_provider_handle_ref_incr(bake_ph);
    srv_ctx->bake_ph = bake_ph;
    uint64_t num_targets;
    ret = bake_probe(bake_ph, 1, &(srv_ctx->bake_tid), &num_targets);
    if(ret != 0) {
        fprintf(stderr, "Error: unable to probe bake server for targets\n");
        ssg_group_destroy(srv_ctx->gid);
        return -1;
    }
    if(num_targets < 1) {
        fprintf(stderr, "Error: unable to find a target on bake provider\n");
        ssg_group_destroy(srv_ctx->gid);
        free(srv_ctx);
        return -1;
    }

    hg_id_t rpc_id;

    rpc_id = MARGO_REGISTER_MPLEX(mid, "mobject_write_op", 
            write_op_in_t, write_op_out_t, mobject_write_op_ult,
            mplex_id, pool);
    margo_register_data_mplex(mid, rpc_id, mplex_id, srv_ctx, NULL);

    rpc_id = MARGO_REGISTER_MPLEX(mid, "mobject_read_op",
            read_op_in_t, read_op_out_t, mobject_read_op_ult,
            mplex_id, pool);
    margo_register_data_mplex(mid, rpc_id, mplex_id, srv_ctx, NULL);

    rpc_id = MARGO_REGISTER_MPLEX(mid, "mobject_shutdown",
            void, void, mobject_shutdown_ult,
            mplex_id, pool);
    margo_register_data_mplex(mid, rpc_id, mplex_id, srv_ctx, NULL);

    margo_push_finalize_callback(mid, mobject_finalize_cb, (void*)srv_ctx);

    *provider = srv_ctx;

    return 0;
}

static hg_return_t mobject_write_op_ult(hg_handle_t h)
{
    hg_return_t ret;

    write_op_in_t in;
    write_op_out_t out;

    /* Deserialize the input from the received handle. */
    ret = margo_get_input(h, &in);
    assert(ret == HG_SUCCESS);

    const struct hg_info* info = margo_get_info(h);
    margo_instance_id mid = margo_hg_handle_get_instance(h);

    server_visitor_args vargs;
    vargs.object_name = in.object_name;
    vargs.pool_name   = in.pool_name;
    vargs.srv_ctx     = margo_registered_data_mplex(mid, info->id, info->target_id);
    if(vargs.srv_ctx == NULL) return HG_OTHER_ERROR;
    vargs.client_addr_str = in.client_addr;
    vargs.client_addr = info->addr;
    vargs.bulk_handle = in.write_op->bulk_handle;

    /* Execute the operation chain */
    //print_write_op(in.write_op, in.object_name);
#ifdef FAKE_CPP_SERVER
    fake_write_op(in.write_op, &vargs);
#else
    core_write_op(in.write_op, &vargs);
#endif

    // set the return value of the RPC
    out.ret = 0;

    ret = margo_respond(h, &out);
    assert(ret == HG_SUCCESS);

    /* Free the input data. */
    ret = margo_free_input(h, &in);
    assert(ret == HG_SUCCESS);

    /* We are not going to use the handle anymore, so we should destroy it. */
    ret = margo_destroy(h);
 
    return ret;
}
DEFINE_MARGO_RPC_HANDLER(mobject_write_op_ult)

/* Implementation of the RPC. */
static hg_return_t mobject_read_op_ult(hg_handle_t h)
{
    hg_return_t ret;

    read_op_in_t in;
    read_op_out_t out;

    /* Deserialize the input from the received handle. */
    ret = margo_get_input(h, &in);
    assert(ret == HG_SUCCESS);

    /* Create a response list matching the input actions */
    read_response_t resp = build_matching_read_responses(in.read_op);

    const struct hg_info* info = margo_get_info(h);
    margo_instance_id mid = margo_hg_handle_get_instance(h);

    server_visitor_args vargs;
    vargs.object_name = in.object_name;
    vargs.pool_name   = in.pool_name;
    vargs.srv_ctx     = margo_registered_data_mplex(mid, info->id, info->target_id);
    if(vargs.srv_ctx == NULL) return HG_OTHER_ERROR;
    vargs.client_addr_str = in.client_addr;
    vargs.client_addr = info->addr;
    vargs.bulk_handle = in.read_op->bulk_handle;

    /* Compute the result. */
    //print_read_op(in.read_op, in.object_name);
#ifdef FAKE_CPP_SERVER
    fake_read_op(in.read_op, &vargs);
#else
    core_read_op(in.read_op, &vargs);
#endif

    out.responses = resp;

    ret = margo_respond(h, &out);
    assert(ret == HG_SUCCESS);

    free_read_responses(resp);

    /* Free the input data. */
    ret = margo_free_input(h, &in);
    assert(ret == HG_SUCCESS);

    /* We are not going to use the handle anymore, so we should destroy it. */
    ret = margo_destroy(h);
    assert(ret == HG_SUCCESS);

    return ret;
}
DEFINE_MARGO_RPC_HANDLER(mobject_read_op_ult)

static void mobject_shutdown_ult(hg_handle_t h)
{
    hg_return_t ret;

    const struct hg_info *info = margo_get_info(h);
    margo_instance_id mid = margo_hg_handle_get_instance(h);

    ret = margo_respond(h, NULL);
    assert(ret == HG_SUCCESS);

    ret = margo_destroy(h);
    assert(ret == HG_SUCCESS);

    margo_finalize(mid);

    return;
}
DEFINE_MARGO_RPC_HANDLER(mobject_shutdown_ult)

static void mobject_finalize_cb(void* data)
{
    mobject_provider_t srv_ctx = (mobject_provider_t)data;

    bake_provider_handle_release(srv_ctx->bake_ph);
    ssg_group_destroy(srv_ctx->gid);

    free(srv_ctx);
}
