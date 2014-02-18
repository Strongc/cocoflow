#include <string.h>

#include <iostream>

#include "cocoflow.h"

#include "simple_rand.h"

using namespace std;

#define TEST_PORT	30917
#define TEST_TIMES	10

#define ASSERT(x) \
do { \
	if (!(x)) \
	{ \
		fprintf(stderr, "[ASSERT]: " #x " failed at " __FILE__ ":%u\n", __LINE__); \
		abort(); \
	} \
} while(0)

class uint32_compare
{
public:
	bool operator()(ccf::uint32 x, ccf::uint32 y) const
	{
		return x>y;
	}
};

class echo_task: public ccf::user_task
{
	static int times;
	static ccf::udp u;
	void run()
	{
		char buf[65536];
		struct sockaddr_in peer;
		size_t len = sizeof(buf);
		ccf::uint64 t = simple_rand()%10000;
		ccf::udp::recv ur(echo_task::u, buf, len);
		await(ur);
		ASSERT(ur.peer_type() == AF_INET);
		peer = ur.peer_addr_ipv4();
		if (++echo_task::times < TEST_TIMES)
			ccf::start(new echo_task());
		cout << "echo_task recv " << len << " from " << ccf::ip_to_str(peer) << endl;
		cout << "echo_task sleep " << t << endl;
		ccf::sleep s(t);
		await(s);
		ccf::udp::send us(echo_task::u, peer, buf, len);
		await(us);
		cout << "echo_task send " << len << " to " << ccf::ip_to_str(peer) << endl;
	}
public:
	static void init()
	{
		ASSERT(u.bind(ccf::ip_to_addr("0.0.0.0", TEST_PORT)) == 0);
	}
};

int echo_task::times = 0;
ccf::udp echo_task::u;

int get_seq_from_buf(const void* buf, size_t size, ccf::uint32* seq)
{
	if (size < sizeof(ccf::uint32))
		return -1;
	*seq = ntohl(*(ccf::uint32*)buf);
	return 0;
}

class seq_task: public ccf::user_task
{
	static int times;
	static ccf::udp u;
	static struct sockaddr_in target;
	ccf::uint32 seq;
	void run()
	{
		char buf[65536];
		ccf::uint32 *pos = (ccf::uint32 *)buf;
		*pos = htonl(this->seq);
		int add_len = simple_rand()%100;
		ccf::udp::send us(seq_task::u, seq_task::target, buf, sizeof(ccf::uint32) + add_len);
		await(us);
		cout << "seq_task send " << sizeof(ccf::uint32) + add_len << ", seq = " << this->seq << endl;
		size_t len = sizeof(buf);
		ccf::udp::recv_by_seq_u32 ur(seq_task::u, buf, len, this->seq);
		await(ur);
		cout << "seq_task recv " << len << ", seq = " << this->seq << endl;
		if (++seq_task::times == TEST_TIMES)
			exit(0);
	}
public:
	static void init()
	{
		seq_task::target = ccf::ip_to_addr("127.0.0.1", TEST_PORT);
		ASSERT(seq_task::u.bind<uint32_compare>(get_seq_from_buf) == 0);
	}
	seq_task(ccf::uint32 seq) : seq(seq) {}
};

int seq_task::times = 0;
ccf::udp seq_task::u;
struct sockaddr_in seq_task::target;

class main_task: public ccf::user_task
{
	void run()
	{
		echo_task::init();
		seq_task::init();
		ccf::start(new echo_task());
		for (int i=0; i<TEST_TIMES; i++)
			ccf::start(new seq_task(i));
	}
};

int main()
{
	ccf::event_task::init(100);
	ccf::user_task::init(100);
	
	//ccf::set_debug(stderr);
	
	main_task tMain;
	ccf::cocoflow(tMain);
	return 0;
}
