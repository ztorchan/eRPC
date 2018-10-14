#include "client_tests.h"

void req_handler(ReqHandle *, void *);  // Forward declaration

/// Request handler for foreground testing
auto reg_info_vec_fg = {
    ReqFuncRegInfo(kTestReqType, req_handler, ReqFuncType::kForeground)};

/// Request handler for background testing
auto reg_info_vec_bg = {
    ReqFuncRegInfo(kTestReqType, req_handler, ReqFuncType::kBackground)};

/// Per-thread application context
class AppContext : public BasicAppContext {};

/// Configuration for controlling the test
size_t config_num_sessions;      ///< Number of sessions created by client
size_t config_num_bg_threads;    ///< Number of background threads
size_t config_rpcs_per_session;  ///< Number of Rpcs per session per iteration
size_t config_msg_size;  ///< The size of the request and response messages

/// The common request handler for all subtests. Copies the request message to
/// the response.
void req_handler(ReqHandle *req_handle, void *_context) {
  auto *context = static_cast<AppContext *>(_context);
  assert(!context->is_client);

  if (config_num_bg_threads > 0) {
    assert(context->rpc->in_background());
  }

  const MsgBuffer *req_msgbuf = req_handle->get_req_msgbuf();
  size_t resp_size = req_msgbuf->get_data_size();
  Rpc<CTransport>::resize_msg_buffer(&req_handle->pre_resp_msgbuf, resp_size);
  memcpy(req_handle->pre_resp_msgbuf.buf, req_msgbuf->buf, resp_size);
  req_handle->prealloc_used = true;

  context->rpc->enqueue_response(req_handle);
}

/// The common continuation function for all subtests. This checks that the
/// request buffer is identical to the response buffer, and increments the
/// number of responses in the context.
void cont_func(void *_context, size_t tag) {
  auto *context = static_cast<AppContext *>(_context);
  ASSERT_EQ(context->resp_msgbufs[tag].get_data_size(), config_msg_size);

  for (size_t i = 0; i < config_msg_size; i++) {
    ASSERT_EQ(context->resp_msgbufs[tag].buf[i], static_cast<uint8_t>(tag));
  }

  assert(context->is_client);
  context->num_rpc_resps++;
}

/// The generic test function that issues \p config_rpcs_per_session Rpcs
/// on each of \p config_num_sessions sessions, for multiple iterations.
///
/// The second \p size_t argument exists only because the client thread function
/// template in client_tests.h requires it.
void generic_test_func(Nexus *nexus, size_t) {
  // Create the Rpc and connect the session
  AppContext context;
  client_connect_sessions(nexus, context, config_num_sessions,
                          basic_sm_handler);

  Rpc<CTransport> *rpc = context.rpc;
  int *session_num_arr = context.session_num_arr;

  // Pre-create MsgBuffers so we can test reuse and resizing
  size_t tot_reqs_per_iter = config_num_sessions * config_rpcs_per_session;
  context.req_msgbufs.resize(tot_reqs_per_iter);
  context.resp_msgbufs.resize(tot_reqs_per_iter);
  for (size_t i = 0; i < tot_reqs_per_iter; i++) {
    context.req_msgbufs[i] =
        rpc->alloc_msg_buffer_or_die(rpc->get_max_data_per_pkt());
    context.resp_msgbufs[i] =
        rpc->alloc_msg_buffer_or_die(rpc->get_max_data_per_pkt());
  }

  // The main request-issuing loop
  for (size_t iter = 0; iter < 2; iter++) {
    context.num_rpc_resps = 0;

    test_printf("Client: Iteration %zu.\n", iter);
    size_t iter_req_i = 0;  // Request MsgBuffer index in an iteration

    for (size_t sess_i = 0; sess_i < config_num_sessions; sess_i++) {
      for (size_t w_i = 0; w_i < config_rpcs_per_session; w_i++) {
        assert(iter_req_i < tot_reqs_per_iter);
        MsgBuffer &cur_req_msgbuf = context.req_msgbufs[iter_req_i];

        rpc->resize_msg_buffer(&cur_req_msgbuf, config_msg_size);
        for (size_t i = 0; i < config_msg_size; i++) {
          cur_req_msgbuf.buf[i] = static_cast<uint8_t>(iter_req_i);
        }

        rpc->enqueue_request(session_num_arr[sess_i], kTestReqType,
                             &cur_req_msgbuf, &context.resp_msgbufs[iter_req_i],
                             cont_func, iter_req_i);

        iter_req_i++;
      }
    }

    wait_for_rpc_resps_or_timeout(context, tot_reqs_per_iter);
    assert(context.num_rpc_resps == tot_reqs_per_iter);
  }

  // Free the MsgBuffers
  for (auto &mb : context.req_msgbufs) rpc->free_msg_buffer(mb);
  for (auto &mb : context.resp_msgbufs) rpc->free_msg_buffer(mb);

  // Disconnect the sessions
  for (size_t sess_i = 0; sess_i < config_num_sessions; sess_i++) {
    rpc->destroy_session(session_num_arr[sess_i]);
  }

  rpc->run_event_loop(kTestEventLoopMs);

  // Free resources
  delete rpc;
  client_done = true;
}

void launch_helper() {
  auto &reg_info_vec =
      config_num_bg_threads == 0 ? reg_info_vec_fg : reg_info_vec_bg;
  launch_server_client_threads(config_num_sessions, config_num_bg_threads,
                               generic_test_func, reg_info_vec,
                               ConnectServers::kFalse, 0.0);
}

TEST(OneSmallRpc, Foreground) {
  config_num_sessions = 1;
  config_num_bg_threads = 0;
  config_rpcs_per_session = 1;
  config_msg_size = Rpc<CTransport>::get_max_data_per_pkt();
  launch_helper();
}

TEST(OneSmallRpc, Background) {
  config_num_sessions = 1;
  config_num_bg_threads = 1;
  config_rpcs_per_session = 1;
  config_msg_size = Rpc<CTransport>::get_max_data_per_pkt();
  launch_helper();
}

TEST(MultiSmallRpcOneSession, Foreground) {
  config_num_sessions = 1;
  config_num_bg_threads = 0;
  config_rpcs_per_session = kSessionReqWindow;
  config_msg_size = Rpc<CTransport>::get_max_data_per_pkt();
  launch_helper();
}

TEST(MultiSmallRpcOneSession, Background) {
  config_num_sessions = 1;
  config_num_bg_threads = 2;
  config_rpcs_per_session = kSessionReqWindow;
  config_msg_size = Rpc<CTransport>::get_max_data_per_pkt();
  launch_helper();
}

TEST(MultiSmallRpcMultiSession, Foreground) {
  config_num_sessions = 4;
  config_num_bg_threads = 0;
  config_rpcs_per_session = kSessionReqWindow;
  config_msg_size = Rpc<CTransport>::get_max_data_per_pkt();
  launch_helper();
}

TEST(MultiSmallRpcMultiSession, Background) {
  config_num_sessions = 4;
  config_num_bg_threads = 3;
  config_rpcs_per_session = kSessionReqWindow;
  config_msg_size = Rpc<CTransport>::get_max_data_per_pkt();
  launch_helper();
}

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
