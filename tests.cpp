#include <gtest/gtest.h>

#include "helpers.h"

TEST(basic, approx_size)
{
	SPSCQueue<int, 8> queue;
	ASSERT_TRUE(queue.TryPush(2));
	ASSERT_TRUE(queue.TryPush(2));
	ASSERT_TRUE(queue.TryPush(2));
	int out;
	ASSERT_TRUE(queue.TryPop(out));
	ASSERT_EQ(queue.GetApproxSize(), 2);
}

TEST(basic, push_pop)
{
	SPSCQueue<int, 16> queue;
	ASSERT_TRUE(queue.TryPush(1));
	ASSERT_TRUE(queue.TryPush(2));
	int out;
	ASSERT_TRUE(queue.TryPop(out) && out == 1);
	ASSERT_TRUE(queue.TryPop(out) && out == 2);
}

TEST(basic, full)
{
	SPSCQueue<int, 8> queue;
	for (size_t i = 1; i <= 8; ++i)
	{
		ASSERT_TRUE(queue.TryPush(i));
	}

	ASSERT_FALSE(queue.TryPush(9));
	ASSERT_FALSE(queue.TryPush(10));

	int out;
	ASSERT_TRUE(queue.TryPop(out) && out == 1);
	ASSERT_TRUE(queue.TryPush(9));
	ASSERT_FALSE(queue.TryPush(10));
}

int main(int argc, char **argv)
{
	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}