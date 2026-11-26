// SPDX-License-Identifier: Apache-2.0
#include "mosaico_storage.h"

#include <string.h>
#include <sys/stat.h>

#include <mutex>

#include "bsp/esp_mosaico.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_blockdev.h"
#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_nand_blockdev.h"
// Only LFS_DISK_VERSION is used from the raw header; the volume is mounted
// through the VFS alone.
#define ESP_LITTLEFS_RAW_API 1
#include "lfs.h"
#include "spi_nand_flash.h"

namespace {
const char *TAG = "storage";
constexpr int CLOCK_HZ = 40 * 1000 * 1000;
constexpr size_t PROBE = 4096;
constexpr uint32_t DHARA_SAFETY_PAGES = 8u << 6;  // DHARA_MAX_RETRIES << log2(64 pages per block)

std::recursive_mutex lock;
// Off: on this board Vibe Mode keeps its LittleFS volume behind its own
// closed logical layer (mosaico_nand_storage, 59534 x 2048-byte blocks), not
// spi_nand_flash's wear levelling (which sees 30178 pages here), so nothing
// here can write to it safely. The read-only probe stays for reference.
constexpr bool ENABLED = false;
bool ready = false, writable = false, paused = false, failed = !ENABLED;
char status[96] = "Board storage is Vibe Mode's own format";
spi_device_handle_t spi = nullptr;
esp_blockdev_handle_t flash = nullptr, wl = nullptr, mounted = nullptr;

// What was found the first time, so later mounts (after a pause) go straight there.
struct {
    bool known = false, on_wl = false;
    uint8_t gc = 0;
    uint32_t version = 0;
} plan;

struct Super {
    bool found = false;
    uint32_t version = 0, block_size = 0, block_count = 0;
};

uint32_t le32(const uint8_t *p)
{
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

// LittleFS keeps its superblock in blocks 0 and 1: the name "littlefs" and,
// a tag later, version, block size and block count (little endian).
Super find_super(esp_blockdev_handle_t dev, uint64_t block_size)
{
    Super s;
    uint8_t *buf = static_cast<uint8_t *>(heap_caps_malloc(PROBE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (!buf) {
        return s;
    }
    for (uint64_t at : {
                uint64_t(0), block_size
            }) {
        if (at + PROBE > dev->geometry.disk_size) {
            break;
        }
        if (dev->ops->read(dev, buf, PROBE, at, PROBE) != ESP_OK) {
            continue;
        }
        for (size_t i = 0; i + 32 <= PROBE; ++i) {
            if (memcmp(buf + i, "littlefs", 8)) {
                continue;
            }
            s.found = true;
            s.version = le32(buf + i + 12);
            s.block_size = le32(buf + i + 16);
            s.block_count = le32(buf + i + 20);
            break;
        }
        if (s.found) {
            break;
        }
    }
    heap_caps_free(buf);
    return s;
}

// esp_littlefs's block size for a device: the erase block for raw NAND
// (classic), lcm(read, write) raised to 128 bytes for a logical device.
uint32_t lfs_block_size(esp_blockdev_handle_t dev)
{
    const esp_blockdev_flags_t &f = dev->device_flags;
    if (f.erase_before_write || f.and_type_write) {
        return uint32_t(dev->geometry.erase_size);
    }
    size_t a = dev->geometry.read_size, b = dev->geometry.write_size;
    size_t x = a, y = b;
    while (y) {
        const size_t t = x % y;
        x = y;
        y = t;
    }
    size_t l = x ? a / x * b : 0;
    if (!l) {
        return 0;
    }
    size_t size = l;
    while (size < 128) {
        size += l;
    }
    return uint32_t(size);
}

void close_devices()
{
    if (wl) {
        wl->ops->release(wl);    // releases the flash layer under it too
    } else if (flash) {
        flash->ops->release(flash);
    }
    wl = flash = nullptr;
    if (spi) {
        spi_bus_remove_device(spi);
        spi = nullptr;
        spi_bus_free(BSP_NAND_SPI_HOST);  // gives its DMA channel back (the camera needs one)
    }
}

bool open_flash(uint8_t gc)
{
    if (!spi) {
        gpio_config_t io = {};
        io.pin_bit_mask = BIT64(BSP_NAND_HOLD) | BIT64(BSP_NAND_WP);
        io.mode = GPIO_MODE_OUTPUT;
        gpio_set_level(BSP_NAND_HOLD, 1);
        gpio_set_level(BSP_NAND_WP, 1);
        if (gpio_config(&io) != ESP_OK) {
            return false;
        }
        spi_bus_config_t bus = {};
        bus.mosi_io_num = BSP_NAND_D;
        bus.miso_io_num = BSP_NAND_Q;
        bus.sclk_io_num = BSP_NAND_CLK;
        bus.quadhd_io_num = GPIO_NUM_NC;
        bus.quadwp_io_num = GPIO_NUM_NC;
        bus.max_transfer_sz = 4096 + 64;
        const esp_err_t b = spi_bus_initialize(BSP_NAND_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
        if (b != ESP_OK && b != ESP_ERR_INVALID_STATE) {
            return false;
        }
        spi_device_interface_config_t dev = {};
        dev.clock_speed_hz = CLOCK_HZ;
        dev.mode = 0;
        dev.spics_io_num = BSP_NAND_CS;
        dev.queue_size = 8;
        dev.flags = SPI_DEVICE_HALFDUPLEX;
        if (spi_bus_add_device(BSP_NAND_SPI_HOST, &dev, &spi) != ESP_OK) {
            spi = nullptr;
            spi_bus_free(BSP_NAND_SPI_HOST);
            return false;
        }
    }
    spi_nand_flash_config_t cfg = {};
    cfg.device_handle = spi;
    cfg.gc_factor = gc;  // 0: the library's default (45)
    cfg.io_mode = SPI_NAND_IO_MODE_SIO;  // no quad bootstrap: nothing is written to the chip's registers
    cfg.flags = SPI_DEVICE_HALFDUPLEX;
    return nand_flash_get_blockdev(&cfg, &flash) == ESP_OK && flash;
}

bool open_wl(uint8_t gc)
{
    if (!open_flash(gc)) {
        return false;
    }
    if (spi_nand_flash_wl_get_blockdev(flash, &wl) != ESP_OK || !wl) {
        wl = nullptr;
        return false;
    }
    return true;
}

// The wear-levelling layer's capacity depends on its GC reserve:
// cap - cap / (gc + 1) - safety. The volume was made with Vibe Mode's
// setting, so find the gc that gives exactly the volume's block count.
int gc_for(uint32_t default_pages, uint32_t want_pages)
{
    // The journal capacity, from the default (gc 45) capacity.
    uint32_t cap = 0;
    const uint32_t guess = uint32_t((uint64_t(default_pages) + DHARA_SAFETY_PAGES) * 46 / 45);
    for (uint32_t c = guess > 200 ? guess - 200 : 1; c < guess + 200; ++c)
        if (c - c / 46 - DHARA_SAFETY_PAGES == default_pages) {
            cap = c;
            break;
        }
    if (!cap || want_pages + DHARA_SAFETY_PAGES >= cap) {
        return -1;
    }
    const uint32_t reserve = cap - DHARA_SAFETY_PAGES - want_pages;
    if (!reserve) {
        return -1;
    }
    for (uint32_t g = 1; g < 256; ++g)
        if (cap / (g + 1) == reserve) {
            return int(g);
        }
    return -1;
}

bool mount(esp_blockdev_handle_t dev, bool read_only)
{
    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path = MOSAICO_STORAGE_ROOT;
    conf.blockdev = dev;
    conf.format_if_mount_failed = false;
    conf.read_only = read_only;
    conf.grow_on_mount = false;
    const esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LittleFS mount failed: %s", esp_err_to_name(err));
    }
    return err == ESP_OK;
}

// Finds the volume (read-only), the first time; returns the device to mount.
esp_blockdev_handle_t discover()
{
    if (!open_flash(0)) {
        strlcpy(status, "NAND not found", sizeof(status));
        return nullptr;
    }
    // 1. LittleFS straight on the NAND (it does its own wear levelling).
    Super s = find_super(flash, flash->geometry.erase_size);
    if (s.found && s.block_size == lfs_block_size(flash) && uint64_t(s.block_count) * s.block_size == flash->geometry.disk_size) {
        plan = {true, false, 0, s.version};
        return flash;
    }
    // 2. On the wear-levelling layer (logical pages), as Vibe Mode keeps it.
    if (spi_nand_flash_wl_get_blockdev(flash, &wl) != ESP_OK || !wl) {
        wl = nullptr;
        strlcpy(status, "NAND layout not recognised", sizeof(status));
        return nullptr;
    }
    const uint32_t bs = lfs_block_size(wl);
    s = find_super(wl, bs);
    if (!s.found || s.block_size != bs) {
        ESP_LOGW(TAG, "no LittleFS volume on the NAND (found %d, block %u); nothing written", int(s.found),
                 unsigned(s.block_size));
        strlcpy(status, s.found ? "NAND volume layout not recognised" : "No volume on the NAND", sizeof(status));
        return nullptr;
    }
    const uint32_t pages = uint32_t(wl->geometry.disk_size / bs);
    if (pages == s.block_count) {
        plan = {true, true, 0, s.version};
        return wl;
    }
    const int gc = gc_for(pages, s.block_count);
    ESP_LOGI(TAG, "volume of %u blocks; this layer has %u at the default reserve; Vibe Mode's reserve: %d",
             unsigned(s.block_count), unsigned(pages), gc);
    if (gc < 0) {
        strlcpy(status, "NAND volume size not recognised", sizeof(status));
        return nullptr;
    }
    close_devices();
    if (!open_wl(uint8_t(gc)) || wl->geometry.disk_size / bs != s.block_count) {
        ESP_LOGW(TAG, "the reserve did not give the volume's size; nothing written");
        strlcpy(status, "NAND volume size not recognised", sizeof(status));
        return nullptr;
    }
    plan = {true, true, uint8_t(gc), s.version};
    return wl;
}

bool try_mount()
{
    esp_blockdev_handle_t dev = nullptr;
    if (!plan.known) {
        dev = discover();
    } else if (plan.on_wl ? open_wl(plan.gc) : open_flash(0)) {
        dev = plan.on_wl ? wl : flash;
    }
    if (!dev) {
        close_devices();
        return false;
    }
    const bool same_version = plan.version == LFS_DISK_VERSION;
    if (!mount(dev, !same_version)) {
        strlcpy(status, "The NAND volume did not mount", sizeof(status));
        close_devices();
        return false;
    }
    mounted = dev;
    writable = same_version;
    if (writable) {
        mkdir(MOSAICO_STORAGE_DIR, 0775);
    }
    size_t total = 0, used = 0;
    esp_littlefs_blockdev_info(dev, &total, &used);
    snprintf(status, sizeof(status), "%s: %u of %u MB free", writable ? "NAND" : "NAND (read-only)",
             unsigned((total - used) >> 20), unsigned(total >> 20));
    ESP_LOGI(TAG, "mounted (%s%s): %s", plan.on_wl ? "wear levelling" : "raw", same_version ? "" : ", read-only: other on-disk version",
             status);
    return true;
}

void unmount()
{
    if (!mounted) {
        return;
    }
    esp_vfs_littlefs_unregister_blockdev(mounted);  // releases the device layers too
    if (mounted == wl || mounted == flash) {
        wl = flash = nullptr;
    }
    mounted = nullptr;
    ready = false;
    close_devices();
}
}  // namespace

extern "C" bool mosaico_storage_mount(void)
{
    std::lock_guard<std::recursive_mutex> g(lock);
    if (ready || paused || failed) {
        return ready;
    }
    ready = try_mount();
    if (!ready && !plan.known) {
        failed = true;    // not this NAND's layout: stop probing
    }
    return ready;
}

extern "C" void mosaico_storage_pause(bool pause)
{
    std::lock_guard<std::recursive_mutex> g(lock);
    if (pause == paused) {
        return;
    }
    paused = pause;
    if (pause) {
        unmount();
    }
}

extern "C" bool mosaico_storage_ready(void)
{
    return ready;
}
extern "C" bool mosaico_storage_paused(void)
{
    return paused;
}
extern "C" bool mosaico_storage_writable(void)
{
    return ready && writable;
}
extern "C" const char *mosaico_storage_status(void)
{
    return paused && plan.known ? "In use by the camera" : status;
}

extern "C" unsigned long long mosaico_storage_free_bytes(void)
{
    std::lock_guard<std::recursive_mutex> g(lock);
    size_t total = 0, used = 0;
    if (!ready || !mounted || esp_littlefs_blockdev_info(mounted, &total, &used) != ESP_OK) {
        return 0;
    }
    return total > used ? total - used : 0;
}
