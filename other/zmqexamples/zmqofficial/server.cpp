#define ZMQ_BUILD_DRAFT_API
#include <zmq.hpp>
#include <iostream>
#include <chrono>
#include <thread>
#include <cassert>
#include <cstring>

#if defined(ZMQ_BUILD_DRAFT_API) && ZMQ_VERSION >= ZMQ_MAKE_VERSION(4, 2, 0)

using namespace std;


int main() {
    void* context = zmq_ctx_new();
	assert(context != nullptr);
	int rc = zmq_ctx_set(context, ZMQ_IO_THREADS, 1);
	assert(rc == 0);

	void* radio = zmq_socket(context, ZMQ_RADIO);
	assert(radio != nullptr);

	rc = zmq_connect(radio, "udp://224.0.0.1:28650");
	assert(rc == 0);

	cout << "init success" << endl;

	while (1) {
		string t = "Test string";
		
		zmq_msg_t msg;
		rc = zmq_msg_init(&msg);
		assert(rc == 0);
		rc = zmq_msg_init_size(&msg, t.size());
		assert(rc == 0);
		if (t.size()) {
			memcpy(zmq_msg_data(&msg), t.data(), t.size());
		}

		rc = zmq_msg_set_group(&msg, "telemetry");
		assert(rc == 0);
		//std::string group = "myGroup"; // Replace with your desired group name
    		//publisher.setsockopt(ZMQ_GROUP, group.c_str(), group.length());

		cout << "sending now" << endl;

		int b = zmq_msg_send(&msg, radio, 0);
		cout << "sent bytes - " << b << endl;
		//rc = zmq_msg_init_size(&);
		// Simulate work
        	std::this_thread::sleep_for(1s);
	}

    return 0;
}
#else
int main() {
    std::cout << "ZMQ_BUILD_DRAFT_API not defined or ZMQ version is earlier than 4.2.0. Group join/leave functionality is not available." << std::endl;
    return 0;
}
#endif
