#pragma once

#include <string>
#include <sstream>
#include <iomanip>
#include <openssl/evp.h>

/// SHA-256 哈希，返回十六进制字符串
inline std::string sha256_hex(const std::string& input) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return {};

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;

    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, input.data(), input.size());
    EVP_DigestFinal_ex(ctx, hash, &hash_len);
    EVP_MD_CTX_free(ctx);

    std::ostringstream oss;
    for (unsigned int i = 0; i < hash_len; ++i)
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
    return oss.str();
}

/// 使用 app salt + 用户名 对密码进行哈希
inline std::string hash_password(const std::string& password, const std::string& username) {
    const std::string APP_SALT = "Asio_mRPC_ChatRoom_2026";
    return sha256_hex(APP_SALT + username + password);
}
