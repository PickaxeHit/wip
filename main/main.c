
#include "elf_loader.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_vfs_dev.h"
#include "payload.h"
#include "unaligned.h"


static const char *TAG = "main";


static const elf_loader_symbol_t exports[] = {
        {"puts", (void *) puts},
        {"printf", (void *) printf},
};
static const elf_loader_env_t env = {exports, sizeof(exports) / sizeof(*exports)};

void app_main(void) {
    ESP_LOGI(TAG, "Let's go!\n");
    void *elf = heap_caps_calloc(1, payload_elf_len, MALLOC_CAP_SPIRAM);
    memcpy(elf, (const void *) payload_elf, payload_elf_len);

    elf_loader_ctx_t *ctx = elf_loader_init_load_and_relocate(elf, &env);
    if (!ctx) {
        ESP_LOGI(TAG, "elf_loader_init_load_and_relocate error");
        return;
    }
    if (elf_loader_set_function(ctx, "local_main") != 0) {
        ESP_LOGI(TAG, "elf_loader_set_function error: local_main function not fount");
        elf_loader_free(ctx);
        return;
    }
    ESP_LOGI(TAG, "Running local_main(0x10) function as int local_main(int arg)");
    int r = elf_loader_run(ctx, 0x10);
    ESP_LOGI(TAG, "Result: %i", r);
    elf_loader_free(ctx);

    ESP_LOGI(TAG, "Done!\n");
    return;
}


//void app_main(void) {
//}
