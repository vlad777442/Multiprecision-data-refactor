#define ZMQ_BUILD_DRAFT_API
#include "test_radio_dish_udp.h"
#include "zmq.hpp"

int main (void)
{
    void *ctx = zmq_ctx_new ();
    assert (ctx);

    void *radio = zmq_socket (ctx, ZMQ_RADIO);
    //void *dish = zmq_socket (ctx, ZMQ_DISH);

    int rc = zmq_connect (radio, "udp://192.168.4.102:6600");
    assert (rc == 0);

    //rc = zmq_bind (dish, "udp://192.168.4.102:6600");
    //assert (rc == 0);

    zmq_sleep (1);


    //rc = zmq_close (dish);
    //assert (rc == 0);
    const char* group = "mcast_test";
    const std::string data{"Hello"};
    int message_num = 0;
    while (true){
        

        zmq_msg_t msg;
        zmq_msg_init_data (&msg, data, 255, my_free, NULL);
        zmq_msg_set_group(&msg, group);

        printf( "Sending message %d\n", message_num);
        zmq_sendmsg(radio, &msg, 0); 
        
        message_num++;
        usleep(100);
    }

    rc = zmq_close (radio);
    assert (rc == 0);

    rc = zmq_ctx_term (ctx);
    assert (rc == 0);

    return 0 ;
}
