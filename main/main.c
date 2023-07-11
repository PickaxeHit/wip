
#include "elf_loader.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_vfs_dev.h"
#include "payload.h"
#include "unaligned.h"


static const char *TAG = "main";


//static const ELFLoaderSymbol_t exports[] = {
//        {"puts", (void *) puts},
//        {"printf", (void *) printf},
//};
//static const ELFLoaderEnv_t env = {exports, sizeof(exports) / sizeof(*exports)};
//
//void test_main(void) {
//    ESP_LOGI(TAG, "Let's go!\n");
//    void *elf = heap_caps_calloc(1, payload_elf_len, MALLOC_CAP_SPIRAM);
//    memcpy(elf, (const void *) payload_elf, payload_elf_len);
//
//    ELFLoaderContext_t *ctx = elfLoaderInitLoadAndRelocate(elf, &env);
//    if (!ctx) {
//        ESP_LOGI(TAG, "elfLoaderInitLoadAndRelocate error");
//        return;
//    }
//    if (elfLoaderSetFunc(ctx, "local_main") != 0) {
//        ESP_LOGI(TAG, "elfLoaderSetFunc error: local_main function not fount");
//        elfLoaderFree(ctx);
//        return;
//    }
//    ESP_LOGI(TAG, "Running local_main(0x10) function as int local_main(int arg)");
//    int r = elfLoaderRun(ctx, 0x10);
//    ESP_LOGI(TAG, "Result: %i", r);
//    elfLoaderFree(ctx);
//
//    ESP_LOGI(TAG, "Done!\n");
//    return;
//}


void app_main(void) {
    void *elf = heap_caps_calloc(1, payload_elf_len + 1, MALLOC_CAP_SPIRAM);
    //    memcpy(elf, (const void *) payload_elf, payload_elf_len);
    //    unaligned_copy(elf, payload_elf, payload_elf_len);
    //    unaligned_copy(elf + 1, payload_elf, payload_elf_len);
    unaligned_copy(elf + 1, payload_elf + 1, payload_elf_len);
}
