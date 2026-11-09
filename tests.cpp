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

TEST(base, empty)
{
	queue_co_t q;
	ASSERT_TRUE(q.IsEmpty());
	ASSERT_EQ(q.GetApproxSize(), 0);
	ASSERT_EQ(opCount.creationCount, 0);

	auto el = CountedOpObj<size_t>(1);
	ASSERT_TRUE(q.TryPush(el));
	ASSERT_FALSE(q.IsEmpty());
	ASSERT_EQ(q.GetApproxSize(), 1);
	ASSERT_EQ(opCount.creationCount, 1);
}

TEST(base, push)
{
	queue_sizet_t q;

	for (size_t i = 0; i % 2 ? q.TryPush(i) : q.TryPush(std::move(i)); ++i)
	{
		ASSERT_EQ(q.GetApproxSize(), i + 1);
	}

	ASSERT_EQ(q.GetApproxSize(), q.GetCapacity());

	size_t* p;
	for (size_t i = 0; (p = q.Front()) != nullptr; ++i)
	{
		ASSERT_EQ(*p, i);
		q.Pop();
		ASSERT_EQ(q.GetApproxSize(), q.GetCapacity() - i - 1);
	}
}

TEST(base, pop)
{
	queue_co_t q;
	prepare(q);
	size_t n = q.GetApproxSize();
	for (size_t i = 0; i < n; ++i)
	{
		ASSERT_EQ(q.GetApproxSize(), n - i);
		q.Pop();
	}

	EXPECT_EQ(q.GetApproxSize(), 0);
	ASSERT_TRUE(q.IsEmpty());
	ASSERT_EQ(opCount.dtorCount, n);
}

TEST(coperators, simple_five_rule_count)
{
	queue_co_t b;
	prepare(b);
	size_t n = b.GetApproxSize();

	queue_co_t c;
	prepare(c);
	ASSERT_EQ(opCount, OpCount(0, 0, 0, 0, 0, 0));

	opCount.clearCounters();

	queue_co_t def;
	for (size_t i = 0; i < n / 2; i++)
	{
		ASSERT_TRUE(def.TryEmplace(i));
	}

	queue_co_t def_move;
	CountedOpObj<size_t> co(0);
	opCount.clearCounters();
	for (size_t i = 0; i < n; i++)
	{
		co = CountedOpObj<size_t>(i);
		ASSERT_TRUE(def_move.TryPush(std::move(co)));
	}

	ASSERT_EQ(opCount, OpCount(n, 0, n, 0, n, n));
}

int main(int argc, char **argv)
{
	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}