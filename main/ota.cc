#include "ota.h"
#include "http_get.h"
#include "system_info.h"
#include "settings.h"
#include "assets/lang_config.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cJSON.h>
#include <esp_log.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>
#include <esp_heap_caps.h>
#ifdef SOC_HMAC_SUPPORTED
#include <esp_hmac.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstring>
#include <expected>
#include <strings.h>
#include <system_error>
#include <vector>

#define TAG "Ota"

namespace {

#if CONFIG_GITHUB_OTA_ENABLE
// Firmware comes from GitHub Releases (CheckGithubRelease); the OTA server's firmware
// section is ignored so official images never replace this build.
constexpr bool kUseServerFirmware = false;
#else
constexpr bool kUseServerFirmware = true;
#endif

// For an app partition this is the digest appended to the image (last 32 bytes of the .bin),
// as hex; empty when the partition holds no valid image.
std::string PartitionDigest(const esp_partition_t* partition) {
    uint8_t digest[32];
    if (esp_partition_get_sha256(partition, digest) != ESP_OK) {
        return "";
    }
    char hex[sizeof(digest) * 2 + 1];
    for (size_t i = 0; i < sizeof(digest); i++) {
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    }
    return hex;
}

#if CONFIG_GITHUB_OTA_ENABLE
// True when the next upgrade would overwrite this very image after it failed to start: the
// bootloader marks an update ABORTED when it reboots before MarkCurrentVersionValid() and
// goes back to the previous firmware.
bool WasRolledBack(const std::string& sha256) {
    const esp_partition_t* partition = esp_ota_get_next_update_partition(nullptr);
    esp_ota_img_states_t state;
    if (partition == nullptr || esp_ota_get_state_partition(partition, &state) != ESP_OK ||
        (state != ESP_OTA_IMG_ABORTED && state != ESP_OTA_IMG_INVALID)) {
        return false;
    }
    return strcasecmp(PartitionDigest(partition).c_str(), sha256.c_str()) == 0;
}

// Leading numbers of a release tag: v1.2.3 / v1.2.3-4-gabc -> {1,2,3}; "dev" -> {0,0,0}
std::array<int, 3> ParseReleaseTag(const std::string& tag) {
    std::array<int, 3> parts{};
    size_t i = (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) ? 1 : 0;
    for (int& part : parts) {
        if (i >= tag.size() || !isdigit(static_cast<unsigned char>(tag[i]))) {
            break;
        }
        while (i < tag.size() && isdigit(static_cast<unsigned char>(tag[i]))) {
            part = std::min(part * 10 + (tag[i++] - '0'), 999999);
        }
        if (i >= tag.size() || tag[i] != '.') {
            break;
        }
        i++;
    }
    return parts;
}

// The tag is put into a download URL, so only allow ordinary tag characters
bool IsValidReleaseTag(const std::string& tag) {
    if (tag.empty() || tag.size() > 32) {
        return false;
    }
    return std::all_of(tag.begin(), tag.end(), [](char c) {
        return isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_';
    });
}
#endif

}  // namespace

Ota::Ota() {
#ifdef ESP_EFUSE_BLOCK_USR_DATA
    // USER_DATA may contain board metadata instead of a textual serial number.
    uint8_t serial_number[32] = {0};
    if (esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA, serial_number, 32 * 8) == ESP_OK) {
        const auto data_end = serial_number + sizeof(serial_number);
        const auto text_end = std::find(serial_number, data_end, uint8_t{0});
        const bool printable = std::all_of(
            serial_number, text_end, [](uint8_t byte) { return byte >= 0x20 && byte <= 0x7E; });
        const bool non_blank =
            std::any_of(serial_number, text_end, [](uint8_t byte) { return byte != ' '; });
        const bool zero_padding =
            std::all_of(text_end, data_end, [](uint8_t byte) { return byte == 0; });
        if (printable && non_blank && zero_padding) {
            serial_number_.assign(reinterpret_cast<const char*>(serial_number),
                                  text_end - serial_number);
            has_serial_number_ = true;
        } else if (std::any_of(serial_number, data_end, [](uint8_t byte) { return byte != 0; })) {
            ESP_LOGW(TAG, "Ignoring non-textual or blank eFuse serial number; using activation v1");
        }
    }
#endif
}

Ota::~Ota() {
}

std::string Ota::GetCheckVersionUrl() {
    Settings settings("wifi", false);
    std::string url = settings.GetString("ota_url");
    if (url.empty()) {
        url = CONFIG_OTA_URL;
    }
    return url;
}

std::unique_ptr<Http> Ota::SetupHttp() {
    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    auto http = network->CreateHttp(0);
    auto user_agent = SystemInfo::GetUserAgent();
    http->SetHeader("Activation-Version", has_serial_number_ ? "2" : "1");
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Client-Id", board.GetUuid());
    if (has_serial_number_) {
        http->SetHeader("Serial-Number", serial_number_.c_str());
        ESP_LOGI(TAG, "Setup HTTP, User-Agent: %s, Serial-Number: %s", user_agent.c_str(), serial_number_.c_str());
    }
    http->SetHeader("User-Agent", user_agent);
    http->SetHeader("Accept-Language", Lang::CODE);
    http->SetHeader("Content-Type", "application/json");

    return http;
}

/* 
 * Specification: https://ccnphfhqs21z.feishu.cn/wiki/FjW6wZmisimNBBkov6OcmfvknVd
 */
NetworkResult<> Ota::CheckVersion() {
    auto& board = Board::GetInstance();

    // Check if there is a new firmware version available
#if CONFIG_GITHUB_OTA_ENABLE
    // Shown on screen: the GitHub release tag ("dev" for local builds). PROJECT_VER stays
    // the upstream version that is still reported to the OTA server.
    current_version_ = XIAOZHI_FW_TAG;
#else
    current_version_ = esp_app_get_description()->version;
#endif
    ESP_LOGI(TAG, "Current version: %s", current_version_.c_str());

    std::string url = GetCheckVersionUrl();
    if (url.length() < 10) {
        ESP_LOGE(TAG, "Check version URL is not properly set");
        return std::unexpected(NetworkError::InvalidArgument());
    }

    auto http = SetupHttp();

    std::string data = board.GetSystemInfoJson();
    std::string method = data.length() > 0 ? "POST" : "GET";
    http->SetContent(std::move(data));

    if (auto opened = http->Open(method, url); !opened) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", opened.error().ToString().c_str());
        return opened;
    }

    auto status_code = http->GetStatusCode();
    if (!status_code) {
        ESP_LOGE(TAG, "Failed to read HTTP status: %s", status_code.error().ToString().c_str());
        return std::unexpected(status_code.error());
    }
    if (*status_code != 200) {
        ESP_LOGE(TAG, "Failed to check version, status code: %d", *status_code);
        return std::unexpected(NetworkError::HttpFailed(*status_code));
    }

    data = http->ReadAll();
    http->Close();

    // Response: { "firmware": { "version": "1.0.0", "url": "http://" } }
    // Parse the JSON response and check if the version is newer
    // If it is, set has_new_version_ to true and store the new version and URL
    
    cJSON *root = cJSON_Parse(data.c_str());
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        return std::unexpected(NetworkError::ProtocolError());
    }

    has_activation_code_ = false;
    has_activation_challenge_ = false;
    cJSON *activation = cJSON_GetObjectItem(root, "activation");
    if (cJSON_IsObject(activation)) {
        cJSON* message = cJSON_GetObjectItem(activation, "message");
        if (cJSON_IsString(message)) {
            activation_message_ = message->valuestring;
        }
        cJSON* code = cJSON_GetObjectItem(activation, "code");
        if (cJSON_IsString(code)) {
            activation_code_ = code->valuestring;
            has_activation_code_ = true;
        }
        cJSON* challenge = cJSON_GetObjectItem(activation, "challenge");
        if (cJSON_IsString(challenge)) {
            activation_challenge_ = challenge->valuestring;
            has_activation_challenge_ = true;
        }
        cJSON* timeout_ms = cJSON_GetObjectItem(activation, "timeout_ms");
        if (cJSON_IsNumber(timeout_ms)) {
            activation_timeout_ms_ = timeout_ms->valueint;
        }
    }

    has_mqtt_config_ = false;
    cJSON *mqtt = cJSON_GetObjectItem(root, "mqtt");
    if (cJSON_IsObject(mqtt)) {
        Settings settings("mqtt", true);
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, mqtt) {
            if (cJSON_IsString(item)) {
                if (settings.GetString(item->string) != item->valuestring) {
                    settings.SetString(item->string, item->valuestring);
                }
            } else if (cJSON_IsNumber(item)) {
                if (settings.GetInt(item->string) != item->valueint) {
                    settings.SetInt(item->string, item->valueint);
                }
            }
        }
        has_mqtt_config_ = true;
    } else {
        ESP_LOGI(TAG, "No mqtt section found !");
    }

    has_websocket_config_ = false;
    cJSON *websocket = cJSON_GetObjectItem(root, "websocket");
    if (cJSON_IsObject(websocket)) {
        Settings settings("websocket", true);
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, websocket) {
            if (cJSON_IsString(item)) {
                if (settings.GetString(item->string) != item->valuestring) {
                    settings.SetString(item->string, item->valuestring);
                }
            } else if (cJSON_IsNumber(item)) {
                if (settings.GetInt(item->string) != item->valueint) {
                    settings.SetInt(item->string, item->valueint);
                }
            }
        }
        has_websocket_config_ = true;
    } else {
        ESP_LOGI(TAG, "No websocket section found!");
    }

    has_server_time_ = false;
    cJSON *server_time = cJSON_GetObjectItem(root, "server_time");
    if (cJSON_IsObject(server_time)) {
        cJSON *timestamp = cJSON_GetObjectItem(server_time, "timestamp");
        cJSON *timezone_offset = cJSON_GetObjectItem(server_time, "timezone_offset");
        
        if (cJSON_IsNumber(timestamp)) {
            // 设置系统时间
            struct timeval tv;
            double ts = timestamp->valuedouble;
            
            // 如果有时区偏移，计算本地时间
            if (cJSON_IsNumber(timezone_offset)) {
                ts += (timezone_offset->valueint * 60 * 1000); // 转换分钟为毫秒
            }
            
            tv.tv_sec = (time_t)(ts / 1000);  // 转换毫秒为秒
            tv.tv_usec = (suseconds_t)((long long)ts % 1000) * 1000;  // 剩余的毫秒转换为微秒
            settimeofday(&tv, NULL);
            has_server_time_ = true;
        }
    } else {
        ESP_LOGW(TAG, "No server_time section found!");
    }

    has_new_version_ = false;
    firmware_sha256_.clear();
    cJSON *firmware = cJSON_GetObjectItem(root, "firmware");
    if (cJSON_IsObject(firmware)) {
        cJSON *version = cJSON_GetObjectItem(firmware, "version");
        if (cJSON_IsString(version)) {
            firmware_version_ = version->valuestring;
        }
        cJSON *url = cJSON_GetObjectItem(firmware, "url");
        if (cJSON_IsString(url)) {
            firmware_url_ = url->valuestring;
        }

        if (kUseServerFirmware && cJSON_IsString(version) && cJSON_IsString(url)) {
            // Check if the version is newer, for example, 0.1.0 is newer than 0.0.1
            has_new_version_ = IsNewVersionAvailable(current_version_, firmware_version_);
            if (has_new_version_) {
                ESP_LOGI(TAG, "New version available: %s", firmware_version_.c_str());
            } else {
                ESP_LOGI(TAG, "Current is the latest version");
            }
            // If the force flag is set to 1, the given version is forced to be installed
            cJSON *force = cJSON_GetObjectItem(firmware, "force");
            if (cJSON_IsNumber(force) && force->valueint == 1) {
                has_new_version_ = true;
            }
        }
    } else {
        ESP_LOGW(TAG, "No firmware section found!");
    }

    cJSON_Delete(root);
    return {};
}

void Ota::MarkCurrentVersionValid() {
    auto partition = esp_ota_get_running_partition();
    if (strcmp(partition->label, "factory") == 0) {
        ESP_LOGI(TAG, "Running from factory partition, skipping");
        return;
    }

    ESP_LOGI(TAG, "Running partition: %s", partition->label);
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(partition, &state) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get state of partition");
        return;
    }

    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "Marking firmware as valid");
        esp_ota_mark_app_valid_cancel_rollback();
    }
}

bool Ota::Upgrade(const std::string& firmware_url, std::function<void(int progress, size_t speed)> callback,
                  const std::string& expected_sha256) {
    ESP_LOGI(TAG, "Upgrading firmware from %s", firmware_url.c_str());
    esp_ota_handle_t update_handle = 0;
    auto update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "Failed to get update partition");
        return false;
    }

    ESP_LOGI(TAG, "Writing to partition %s at offset 0x%lx", update_partition->label, update_partition->address);
    bool image_header_checked = false;
    std::string image_header;

    std::string error;
    auto http = OpenHttpGet(firmware_url, error, SystemInfo::GetUserAgent());
    if (!http) {
        ESP_LOGE(TAG, "Failed to get firmware: %s", error.c_str());
        return false;
    }

    size_t content_length = http->GetBodyLength();
    if (content_length == 0) {
        ESP_LOGE(TAG, "Failed to get content length");
        return false;
    }

    constexpr size_t PAGE_SIZE = 4096;
    char* buffer = (char*)heap_caps_malloc(PAGE_SIZE, MALLOC_CAP_INTERNAL);
    if (buffer == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate buffer");
        return false;
    }

    size_t buffer_offset = 0;  // Current data size in buffer
    size_t total_read = 0, recent_read = 0;
    auto last_calc_time = esp_timer_get_time();
    while (true) {
        auto ret = http->Read(buffer + buffer_offset, PAGE_SIZE - buffer_offset);
        if (!ret) {
            ESP_LOGE(TAG, "Failed to read HTTP data: %s", ret.error().ToString().c_str());
            heap_caps_free(buffer);
            return false;
        }
        int n = *ret;

        // Calculate speed and progress every second
        recent_read += n;
        total_read += n;
        buffer_offset += n;
        if (esp_timer_get_time() - last_calc_time >= 1000000 || n == 0) {
            size_t progress = total_read * 100 / content_length;
            ESP_LOGI(TAG, "Progress: %u%% (%u/%u), Speed: %uB/s", progress, total_read, content_length, recent_read);
            if (callback) {
                callback(progress, recent_read);
            }
            last_calc_time = esp_timer_get_time();
            recent_read = 0;
        }

        if (!image_header_checked) {
            image_header.append(buffer, buffer_offset);
            if (image_header.size() >= sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t)) {
                esp_app_desc_t new_app_info;
                memcpy(&new_app_info, image_header.data() + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t), sizeof(esp_app_desc_t));

                if (esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &update_handle)) {
                    esp_ota_abort(update_handle);
                    ESP_LOGE(TAG, "Failed to begin OTA");
                    heap_caps_free(buffer);
                    return false;
                }

                image_header_checked = true;
                std::string().swap(image_header);
            }
        }

        // Write to flash when buffer is full (4KB) or it's the last chunk
        bool is_last_chunk = (n == 0);
        if (buffer_offset == PAGE_SIZE || (is_last_chunk && buffer_offset > 0)) {
            auto err = esp_ota_write(update_handle, buffer, buffer_offset);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to write OTA data: %s", esp_err_to_name(err));
                esp_ota_abort(update_handle);
                heap_caps_free(buffer);
                return false;
            }

            buffer_offset = 0;
        }

        if (is_last_chunk) {
            break;
        }
    }
    http->Close();
    heap_caps_free(buffer);

    esp_err_t err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "Image validation failed, image is corrupted");
        } else {
            ESP_LOGE(TAG, "Failed to end OTA: %s", esp_err_to_name(err));
        }
        return false;
    }

    if (!expected_sha256.empty()) {
        std::string hex = PartitionDigest(update_partition);
        if (hex.empty()) {
            ESP_LOGE(TAG, "Failed to read image digest");
            return false;
        }
        if (strcasecmp(hex.c_str(), expected_sha256.c_str()) != 0) {
            ESP_LOGE(TAG, "Image digest mismatch: %s, expected %s", hex.c_str(),
                     expected_sha256.c_str());
            return false;
        }
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set boot partition: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "Firmware upgrade successful");
    return true;
}

bool Ota::StartUpgrade(std::function<void(int progress, size_t speed)> callback) {
    return Upgrade(firmware_url_, callback, firmware_sha256_);
}

bool Ota::CheckGithubRelease() {
#if CONFIG_GITHUB_OTA_ENABLE
    const std::string current = XIAOZHI_FW_TAG;
    Settings settings("github_ota", true);
    // Remember the release this firmware came from. A local build ("dev") flashed afterwards
    // is then not replaced by that same release again, only by a newer one.
    if (current != "dev" && settings.GetString("installed") != current) {
        settings.SetString("installed", current);
    }

    // Mirror first (direct github.com is often slow or blocked), then github.com itself.
    // The firmware is later downloaded from the same source that answered.
    const std::string base = "https://github.com/" CONFIG_GITHUB_OTA_REPO "/releases/";
    const std::string sources[] = {CONFIG_GITHUB_OTA_MIRROR, ""};
    std::string source;
    std::unique_ptr<Http> http;
    for (const auto& prefix : sources) {
        std::string error;
        http = OpenHttpGet(prefix + base + "latest/download/" BOARD_NAME ".json", error,
                           SystemInfo::GetUserAgent());
        if (http) {
            source = prefix;
            break;
        }
        ESP_LOGW(TAG, "GitHub release check via %s failed: %s",
                 prefix.empty() ? "github.com" : prefix.c_str(), error.c_str());
    }
    if (!http) {
        return false;
    }
    std::string body = http->ReadAll();
    http->Close();

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "GitHub release: invalid " BOARD_NAME ".json");
        return false;
    }
    cJSON* version = cJSON_GetObjectItem(root, "version");
    cJSON* sha256 = cJSON_GetObjectItem(root, "sha256");
    std::string latest = cJSON_IsString(version) ? version->valuestring : "";
    std::string digest = cJSON_IsString(sha256) ? sha256->valuestring : "";
    cJSON_Delete(root);
    if (!IsValidReleaseTag(latest) || digest.size() != 64) {
        ESP_LOGW(TAG, "GitHub release: incomplete " BOARD_NAME ".json");
        return false;
    }

    if (latest == current || ParseReleaseTag(latest) <= ParseReleaseTag(current)) {
        ESP_LOGI(TAG, "Firmware %s is up to date (latest release %s)", current.c_str(), latest.c_str());
        return false;
    }
    if (settings.GetString("installed") == latest) {
        ESP_LOGI(TAG, "Release %s was installed before; keeping local build %s", latest.c_str(),
                 current.c_str());
        return false;
    }
    // Installing it again would only crash and roll back again on every boot
    if (WasRolledBack(digest)) {
        ESP_LOGW(TAG, "Release %s failed to start and was rolled back; waiting for a newer release",
                 latest.c_str());
        return false;
    }

    ESP_LOGI(TAG, "New release %s available (current %s)", latest.c_str(), current.c_str());
    firmware_version_ = latest;
    firmware_url_ = source + base + "download/" + latest + "/" BOARD_NAME ".bin";
    firmware_sha256_ = digest;
    has_new_version_ = true;
    return true;
#else
    return false;
#endif
}

bool Ota::CheckGithubAssets() {
#if CONFIG_GITHUB_OTA_ENABLE
    const esp_partition_t* partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, "assets");
    if (partition == nullptr) {
        return false;
    }

    // A fixed pre-release, so it never becomes the latest release that firmware is taken from
    const std::string base = "https://github.com/" CONFIG_GITHUB_OTA_REPO
                             "/releases/download/assets/" BOARD_NAME "-assets.";
    const std::string sources[] = {CONFIG_GITHUB_OTA_MIRROR, ""};
    std::string source;
    std::unique_ptr<Http> http;
    for (const auto& prefix : sources) {
        std::string error;
        http = OpenHttpGet(prefix + base + "json", error, SystemInfo::GetUserAgent());
        if (http) {
            source = prefix;
            break;
        }
        ESP_LOGW(TAG, "GitHub assets check via %s failed: %s",
                 prefix.empty() ? "github.com" : prefix.c_str(), error.c_str());
        // GitHub has no pack for this board; asking github.com directly would only slow down boot
        if (error == "HTTP 404") {
            return false;
        }
    }
    if (!http) {
        return false;
    }
    std::string body = http->ReadAll();
    http->Close();

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "GitHub assets: invalid " BOARD_NAME "-assets.json");
        return false;
    }
    cJSON* sha256 = cJSON_GetObjectItem(root, "sha256");
    cJSON* size = cJSON_GetObjectItem(root, "size");
    std::string digest = cJSON_IsString(sha256) ? sha256->valuestring : "";
    double length = cJSON_IsNumber(size) ? size->valuedouble : 0;
    cJSON_Delete(root);
    if (digest.size() != 64 || length <= 12 || length > partition->size) {
        ESP_LOGW(TAG, "GitHub assets: incomplete " BOARD_NAME "-assets.json");
        return false;
    }

    // The digest names the pack. Each new pack is installed once, so a pack pushed from the
    // xiaozhi.me console or flashed by hand stays until another one is uploaded to the release.
    Settings settings("github_ota", true);
    if (strcasecmp(settings.GetString("assets").c_str(), digest.c_str()) == 0) {
        ESP_LOGI(TAG, "Assets %.8s is up to date", digest.c_str());
        return false;
    }

    ESP_LOGI(TAG, "New assets %.8s available", digest.c_str());
    assets_url_ = source + base + "bin";
    assets_sha256_ = digest;
    return true;
#else
    return false;
#endif
}

void Ota::MarkGithubAssetsInstalled() {
    if (assets_sha256_.empty()) {
        return;
    }
    Settings settings("github_ota", true);
    settings.SetString("assets", assets_sha256_);
}


// Versions are compared as dot-separated decimal numbers. Parsing must never throw: exceptions are
// disabled in this project, so a malformed string from the server would otherwise terminate the
// firmware instead of being reported as an error.
std::expected<std::vector<int>, std::string> Ota::ParseVersion(const std::string& version) {
    std::vector<int> version_numbers;
    size_t start = 0;

    while (true) {
        size_t end = version.find('.', start);
        std::string segment =
            version.substr(start, end == std::string::npos ? std::string::npos : end - start);

        int value = 0;
        auto [ptr, ec] = std::from_chars(segment.data(), segment.data() + segment.size(), value);
        if (segment.empty() || ec != std::errc() || ptr != segment.data() + segment.size()) {
            return std::unexpected("invalid version segment \"" + segment + "\"");
        }
        version_numbers.push_back(value);

        if (end == std::string::npos) {
            return version_numbers;
        }
        start = end + 1;
    }
}

bool Ota::IsNewVersionAvailable(const std::string& currentVersion, const std::string& newVersion) {
    auto current = ParseVersion(currentVersion);
    if (!current) {
        ESP_LOGW(TAG, "Cannot parse current version \"%s\": %s", currentVersion.c_str(),
                 current.error().c_str());
        return false;
    }

    auto newer = ParseVersion(newVersion);
    if (!newer) {
        // Never report an update we cannot compare, otherwise a malformed version string would
        // trigger a firmware downgrade or an endless upgrade loop.
        ESP_LOGW(TAG, "Ignoring firmware version \"%s\": %s", newVersion.c_str(),
                 newer.error().c_str());
        return false;
    }

    for (size_t i = 0; i < std::min(current->size(), newer->size()); ++i) {
        if ((*newer)[i] > (*current)[i]) {
            return true;
        } else if ((*newer)[i] < (*current)[i]) {
            return false;
        }
    }

    return newer->size() > current->size();
}

std::string Ota::GetActivationPayload() {
    if (!has_serial_number_) {
        return "{}";
    }

    std::string hmac_hex;
#ifdef SOC_HMAC_SUPPORTED
    uint8_t hmac_result[32]; // SHA-256 输出为32字节
    
    // 使用Key0计算HMAC
    esp_err_t ret = esp_hmac_calculate(HMAC_KEY0, (uint8_t*)activation_challenge_.data(), activation_challenge_.size(), hmac_result);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HMAC calculation failed: %s", esp_err_to_name(ret));
        return "{}";
    }

    for (size_t i = 0; i < sizeof(hmac_result); i++) {
        char buffer[3];
        sprintf(buffer, "%02x", hmac_result[i]);
        hmac_hex += buffer;
    }
#endif

    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "algorithm", "hmac-sha256");
    cJSON_AddStringToObject(payload, "serial_number", serial_number_.c_str());
    cJSON_AddStringToObject(payload, "challenge", activation_challenge_.c_str());
    cJSON_AddStringToObject(payload, "hmac", hmac_hex.c_str());
    auto json_str = cJSON_PrintUnformatted(payload);
    std::string json(json_str);
    cJSON_free(json_str);
    cJSON_Delete(payload);

    ESP_LOGI(TAG, "Activation payload: %s", json.c_str());
    return json;
}

esp_err_t Ota::Activate() {
    if (!has_activation_challenge_) {
        ESP_LOGW(TAG, "No activation challenge found");
        return ESP_FAIL;
    }

    std::string url = GetCheckVersionUrl();
    if (url.back() != '/') {
        url += "/activate";
    } else {
        url += "activate";
    }

    auto http = SetupHttp();

    std::string data = GetActivationPayload();
    http->SetContent(std::move(data));

    if (auto opened = http->Open("POST", url); !opened) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", opened.error().ToString().c_str());
        return ESP_FAIL;
    }

    auto status_code = http->GetStatusCode();
    if (!status_code) {
        ESP_LOGE(TAG, "Failed to read HTTP status: %s", status_code.error().ToString().c_str());
        return ESP_FAIL;
    }
    if (*status_code == 202) {
        return ESP_ERR_TIMEOUT;
    }
    if (*status_code != 200) {
        ESP_LOGE(TAG, "Failed to activate, code: %d, body: %s", *status_code, http->ReadAll().c_str());
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Activation successful");
    return ESP_OK;
}
