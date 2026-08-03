#include "unity.h"
#include "bl_crc32.h"
#include <stdint.h>

void setUp(void){

}

void tearDown(void){

}

void test_crc(void){
  uint32_t output = bl_crc32(0, "123456789", 9);
  TEST_ASSERT_EQUAL_HEX32(0xCBF43926, output);
}


void test_crc_split(void){
  uint32_t output = bl_crc32(bl_crc32(0, "12345", 5), "6789", 4);
  TEST_ASSERT_EQUAL_HEX32(0xCBF43926, output);
}

void test_crc_empty(void){
  uint32_t output = bl_crc32(0, NULL, 0);
  TEST_ASSERT_EQUAL_HEX32(0, output);
}

void test_crc_diff(void){
  uint32_t output1 = bl_crc32(0, "123456789", 9);
  uint32_t output2 = bl_crc32(0, "123456799", 9);
  TEST_ASSERT_FALSE(output1 == output2);
  
}

int main(void){
    UNITY_BEGIN();

    RUN_TEST(test_crc);
    RUN_TEST(test_crc_split);
    RUN_TEST(test_crc_empty);
    RUN_TEST(test_crc_diff);

    return UNITY_END();
}
