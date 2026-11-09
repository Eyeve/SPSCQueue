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
	try
	{
		while (auto* out = q.Front())
		{
			ASSERT_TRUE(out->value <= 4 && out->value >= 0);
			++v[out->value];
			q.Pop();
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

int main(int argc, char **argv)
{
	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}