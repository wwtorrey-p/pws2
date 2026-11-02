#include "cert.h"

#include <vector>

#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"
#include "nvs.h"
#include "generated/cert_assets.h"

static const char* TAG = "cert";

static bool readStr(nvs_handle_t h, const char* key, std::string& out) {
    size_t len = 0;
    if (nvs_get_str(h, key, nullptr, &len) != ESP_OK || len < 2) return false;
    std::vector<char> buf(len);
    if (nvs_get_str(h, key, buf.data(), &len) != ESP_OK) return false;
    out.assign(buf.data());
    return true;
}

static bool generate(std::string& certPem, std::string& keyPem) {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_pk_context key;
    mbedtls_x509write_cert crt;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_pk_init(&key);
    mbedtls_x509write_crt_init(&crt);

    bool ok = false;
    std::vector<unsigned char> certBuf(2048), keyBuf(1024);
    unsigned char serial[8];
    const char* name = "CN=weather-station,O=wx";

    do {
        if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, (const unsigned char*)"wx", 2)) break;
        if (mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY))) break;
        if (mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key), mbedtls_ctr_drbg_random, &drbg)) break;

        mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
        mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
        mbedtls_x509write_crt_set_subject_key(&crt, &key);
        mbedtls_x509write_crt_set_issuer_key(&crt, &key);
        if (mbedtls_x509write_crt_set_subject_name(&crt, name)) break;
        if (mbedtls_x509write_crt_set_issuer_name(&crt, name)) break;
        esp_fill_random(serial, sizeof serial);
        serial[0] &= 0x7f;
        if (mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof serial)) break;
        // Fixed window: the device clock is not trustworthy at first boot.
        if (mbedtls_x509write_crt_set_validity(&crt, "20240101000000", "20740101000000")) break;
        if (mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1)) break;

        if (mbedtls_x509write_crt_pem(&crt, certBuf.data(), certBuf.size(), mbedtls_ctr_drbg_random, &drbg)) break;
        if (mbedtls_pk_write_key_pem(&key, keyBuf.data(), keyBuf.size())) break;
        certPem.assign((char*)certBuf.data());
        keyPem.assign((char*)keyBuf.data());
        ok = true;
    } while (false);

    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&key);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    return ok;
}

bool ensureCert(std::string& certPem, std::string& keyPem) {
    if (tls_assets::load(certPem, keyPem)) {
        ESP_LOGI(TAG, "using embedded local-trust certificate");
        return true;
    }

    nvs_handle_t h;
    if (nvs_open("wx", NVS_READWRITE, &h) != ESP_OK) return false;
    if (readStr(h, "cert", certPem) && readStr(h, "key", keyPem)) {
        nvs_close(h);
        return true;
    }
    ESP_LOGI(TAG, "generating self-signed certificate");
    bool ok = generate(certPem, keyPem) && nvs_set_str(h, "cert", certPem.c_str()) == ESP_OK &&
              nvs_set_str(h, "key", keyPem.c_str()) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}
