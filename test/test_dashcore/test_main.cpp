// 测试程序入口:汇总注册各测试文件的用例。
// pio 会把 test_dashcore/ 下所有 .cpp 编进同一个可执行文件,main 只有一个。
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

void register_obd_protocol_tests(void);
void register_obd_source_tests(void);
void register_van_source_tests(void);
void register_van_replay_tests(void);
void register_van_wire_tests(void);
void register_van_phy_wire_tests(void);
void register_theme_store_tests(void);
void register_image_blob_tests(void);
void register_image_roundtrip_tests(void);
void register_data_service_tests(void);
void register_expression_tests(void);
void register_face_stage_tests(void);

int main(void) {
  UNITY_BEGIN();
  register_obd_protocol_tests();
  register_obd_source_tests();
  register_van_source_tests();
  register_van_replay_tests();
  register_van_wire_tests();
  register_van_phy_wire_tests();
  register_theme_store_tests();
  register_image_blob_tests();
  register_image_roundtrip_tests();
  register_data_service_tests();
  register_expression_tests();
  register_face_stage_tests();
  return UNITY_END();
}
