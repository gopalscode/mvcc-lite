#include "mvcc/kv_store.h"

#include <gtest/gtest.h>

using mvcc::KVStore;
using mvcc::Status;

// ---------------------------------------------------------------------
// Happy path
// ---------------------------------------------------------------------

TEST(KVStoreBasic, PutThenGetReturnsSameValue) {
  KVStore store;
  ASSERT_TRUE(store.Put("name", "gopal").ok());

  std::string value;
  Status s = store.Get("name", &value);
  ASSERT_TRUE(s.ok());
  EXPECT_EQ(value, "gopal");
}

TEST(KVStoreBasic, PutOverwritesExistingValue) {
  KVStore store;
  ASSERT_TRUE(store.Put("k", "v1").ok());
  ASSERT_TRUE(store.Put("k", "v2").ok());

  std::string value;
  ASSERT_TRUE(store.Get("k", &value).ok());
  EXPECT_EQ(value, "v2");
  // Overwrite must not create a second entry.
  EXPECT_EQ(store.Size(), 1u);
}

TEST(KVStoreBasic, DeleteRemovesKey) {
  KVStore store;
  ASSERT_TRUE(store.Put("k", "v").ok());
  ASSERT_TRUE(store.Delete("k").ok());
  EXPECT_FALSE(store.Contains("k"));
  EXPECT_EQ(store.Size(), 0u);
}

TEST(KVStoreBasic, SizeTracksDistinctKeys) {
  KVStore store;
  EXPECT_EQ(store.Size(), 0u);
  store.Put("a", "1");
  store.Put("b", "2");
  store.Put("a", "3");  // overwrite, not a new key
  EXPECT_EQ(store.Size(), 2u);
}

// ---------------------------------------------------------------------
// Edge cases: this is the part it's tempting to skip. Don't.
// ---------------------------------------------------------------------

TEST(KVStoreEdgeCases, GetOnMissingKeyReturnsNotFound) {
  KVStore store;
  std::string value = "unchanged";
  Status s = store.Get("nope", &value);
  EXPECT_TRUE(s.is_not_found());
  // out_value must be left alone on failure.
  EXPECT_EQ(value, "unchanged");
}

TEST(KVStoreEdgeCases, DeleteOnMissingKeyReturnsNotFound) {
  KVStore store;
  EXPECT_TRUE(store.Delete("nope").is_not_found());
}

TEST(KVStoreEdgeCases, GetOnEmptyStoreReturnsNotFound) {
  KVStore store;
  std::string value;
  EXPECT_TRUE(store.Get("anything", &value).is_not_found());
}

TEST(KVStoreEdgeCases, EmptyKeyIsRejectedOnAllOperations) {
  KVStore store;
  std::string value;
  EXPECT_TRUE(store.Put("", "v").is_invalid_argument());
  EXPECT_TRUE(store.Get("", &value).is_invalid_argument());
  EXPECT_TRUE(store.Delete("").is_invalid_argument());
}

TEST(KVStoreEdgeCases, EmptyValueIsAllowed) {
  KVStore store;
  ASSERT_TRUE(store.Put("k", "").ok());
  std::string value = "sentinel";
  ASSERT_TRUE(store.Get("k", &value).ok());
  EXPECT_EQ(value, "");
}

TEST(KVStoreEdgeCases, DeleteThenGetReturnsNotFound) {
  KVStore store;
  store.Put("k", "v");
  store.Delete("k");
  std::string value;
  EXPECT_TRUE(store.Get("k", &value).is_not_found());
}

TEST(KVStoreEdgeCases, ReinsertingAfterDeleteWorks) {
  KVStore store;
  store.Put("k", "v1");
  store.Delete("k");
  ASSERT_TRUE(store.Put("k", "v2").ok());
  std::string value;
  ASSERT_TRUE(store.Get("k", &value).ok());
  EXPECT_EQ(value, "v2");
}

TEST(KVStoreEdgeCases, KeysWithSpecialCharactersAndEmbeddedNulLikeBytes) {
  KVStore store;
  std::string key = "weird key\twith\nwhitespace and \xE2\x9C\x93 utf8";
  std::string val = std::string("bin\0ary", 7);  // embedded NUL, explicit length
  ASSERT_TRUE(store.Put(key, val).ok());
  std::string out;
  ASSERT_TRUE(store.Get(key, &out).ok());
  EXPECT_EQ(out, val);
}

TEST(KVStoreEdgeCases, LargeValueRoundTrips) {
  KVStore store;
  std::string big(1'000'000, 'x');
  ASSERT_TRUE(store.Put("big", big).ok());
  std::string out;
  ASSERT_TRUE(store.Get("big", &out).ok());
  EXPECT_EQ(out.size(), big.size());
  EXPECT_EQ(out, big);
}
