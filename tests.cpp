#include <gtest/gtest.h>

#include "helpers.h"


TEST(traits, default_constructor)
{
	ASSERT_TRUE(std::is_default_constructible< queue_sizet_t >());
	ASSERT_TRUE(std::is_default_constructible< queue_string_t >());
	ASSERT_TRUE(std::is_default_constructible< queue_nc_t >());
	ASSERT_TRUE(std::is_default_constructible< queue_co_t >());
}

TEST(traits, non_copy_constructible)
{
	ASSERT_FALSE(std::is_copy_constructible< queue_sizet_t >());
	ASSERT_FALSE(std::is_copy_constructible< queue_string_t >());
	ASSERT_FALSE(std::is_copy_constructible< queue_nc_t >());
	ASSERT_FALSE(std::is_copy_constructible< queue_co_t >());
}

TEST(traits, move_constructible)
{
	ASSERT_FALSE(std::is_move_constructible< queue_sizet_t >());
	ASSERT_FALSE(std::is_move_constructible< queue_string_t >());
	ASSERT_FALSE(std::is_move_constructible< queue_nc_t >());
	ASSERT_FALSE(std::is_move_constructible< queue_co_t >());
}

TEST(traits, destructible)
{
	ASSERT_TRUE(std::is_destructible< queue_sizet_t >());
	ASSERT_TRUE(std::is_destructible< queue_string_t >());
	ASSERT_TRUE(std::is_destructible< queue_nc_t >());
	ASSERT_TRUE(std::is_destructible< queue_co_t >());
}

TEST(base, ctor)
{
	queue_nc_t q;
	ASSERT_EQ(q.GetApproxSize(), 0);
	ASSERT_EQ(q.GetCapacity(), kDefaultCapacity);

	bool throw_detected = false;
	try
	{
		ASSERT_TRUE(q.TryPush(NoCopy(1)));
		NoCopy c(2);
		ASSERT_TRUE(q.TryPush(std::move(c)));
	} catch (int)
	{
		throw_detected = true;
	}
	ASSERT_FALSE(throw_detected);
	ASSERT_EQ(q.GetApproxSize(), 2);

	try
	{
		NoCopy c(666);
		ASSERT_TRUE(q.TryPush(c));
	} catch (int)
	{
		throw_detected = true;
	}
	ASSERT_TRUE(throw_detected);
	ASSERT_EQ(q.GetApproxSize(), 2);

	ASSERT_TRUE(q.TryPush(NoCopy(0)));
	ASSERT_EQ(q.GetApproxSize(), 3);

	throw_detected = false;
	try
	{
		ASSERT_TRUE(q.TryEmplace(3));
		int c = 4;
		ASSERT_TRUE(q.TryEmplace(c));
	} catch (int)
	{
		throw_detected = true;
	}
	ASSERT_FALSE(throw_detected);
	ASSERT_EQ(q.GetApproxSize(), 5);

	int v[5] = {};
	throw_detected = false;
	NoCopy out;
	try
	{
		while (q.TryPop(out))
		{
			if (out.value <= 4 && out.value >= 0)
				++v[out.value];
		}
	} catch (int)
	{
		throw_detected = true;
	}
	ASSERT_FALSE(throw_detected);

	for (size_t i = 0; i < 5; ++i)
	{
		EXPECT_EQ(v[i], 1);
	}
}

// TEST(basic, approx_size)
// {
// 	SPSCQueue<int, 8> queue;
// 	ASSERT_TRUE(queue.TryPush(2));
// 	ASSERT_TRUE(queue.TryPush(2));
// 	ASSERT_TRUE(queue.TryPush(2));
// 	int out;
// 	ASSERT_TRUE(queue.TryPop(out));
// 	ASSERT_EQ(queue.GetApproxSize(), 2);
// }
//
// TEST(basic, push_pop)
// {
// 	SPSCQueue<int, 16> queue;
// 	ASSERT_TRUE(queue.TryPush(1));
// 	ASSERT_TRUE(queue.TryPush(2));
// 	int out;
// 	ASSERT_TRUE(queue.TryPop(out) && out == 1);
// 	ASSERT_TRUE(queue.TryPop(out) && out == 2);
// }
//
// TEST(basic, full)
// {
// 	SPSCQueue<int, 8> queue;
// 	for (size_t i = 1; i <= 8; ++i)
// 	{
// 		ASSERT_TRUE(queue.TryPush(i));
// 	}
//
// 	ASSERT_FALSE(queue.TryPush(9));
// 	ASSERT_FALSE(queue.TryPush(10));
//
// 	int out;
// 	ASSERT_TRUE(queue.TryPop(out) && out == 1);
// 	ASSERT_TRUE(queue.TryPush(9));
// 	ASSERT_FALSE(queue.TryPush(10));
// }

int main(int argc, char **argv)
{
	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}