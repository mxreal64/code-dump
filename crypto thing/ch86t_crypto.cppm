module;
#include "libsodium/src/libsodium/include/sodium.h"

export module ch86t.crypto;

import <string>;
import <vector>;
import <cstdint>;

export namespace ch86t::crypto {

    struct IdentityBundle {
        std::string username;
        std::string fingerprint;
        std::vector<uint8_t> ed_public_key;
        std::vector<uint8_t> ed_secret_key;
        std::vector<uint8_t> x_public_key;
        std::vector<uint8_t> x_secret_key;
    };

    struct EncryptedPacket {
        std::vector<uint8_t> nonce;
        std::vector<uint8_t> ciphertext;
    };

    bool initialize_secure_runtime() { return sodium_init() >= 0; }

    std::string to_hex(const std::vector<uint8_t>& data) {
        static constexpr char hex_chars[] = "0123456789abcdef";
        std::string result; result.reserve(data.size() * 2);
        for (uint8_t byte : data) {
            result.push_back(hex_chars[(byte >> 4) & 0xF]);
            result.push_back(hex_chars[byte & 0xF]);
        }
        return result;
    }

    std::vector<uint8_t> from_hex(const std::string& hex) {
        if (hex.size() % 2 != 0) return {};
        std::vector<uint8_t> result(hex.size() / 2);
        auto parse = [](char c) -> uint8_t {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
            return 0;
        };
        for (size_t i = 0; i < result.size(); ++i) {
            result[i] = (parse(hex[i * 2]) << 4) | parse(hex[i * 2 + 1]);
        }
        return result;
    }

    void derive_x25519_keys(const std::vector<uint8_t>& ed_pub, const std::vector<uint8_t>& ed_sec,
                            std::vector<uint8_t>& x_pub, std::vector<uint8_t>& x_sec) {
        x_pub.resize(crypto_box_PUBLICKEYBYTES); x_sec.resize(crypto_box_SECRETKEYBYTES);
        crypto_sign_ed25519_pk_to_curve25519(x_pub.data(), ed_pub.data());
        crypto_sign_ed25519_sk_to_curve25519(x_sec.data(), ed_sec.data());
    }

    IdentityBundle generate_identity(const std::string& username) {
        IdentityBundle id{.username = username};
        id.ed_public_key.resize(crypto_sign_PUBLICKEYBYTES);
        id.ed_secret_key.resize(crypto_sign_SECRETKEYBYTES);
        crypto_sign_keypair(id.ed_public_key.data(), id.ed_secret_key.data());
        derive_x25519_keys(id.ed_public_key, id.ed_secret_key, id.x_public_key, id.x_secret_key);

        std::vector<uint8_t> hash(crypto_hash_sha256_BYTES);
        crypto_hash_sha256(hash.data(), id.ed_public_key.data(), id.ed_public_key.size());
        
        static constexpr char chars[] = "0123456789abcdef";
        id.fingerprint = "@" + username + "-" + chars[(hash[0] >> 4) & 0xF] + chars[hash[0] & 0xF] + chars[(hash[1] >> 4) & 0xF] + chars[hash[1] & 0xF];
        return id;
    }

    EncryptedPacket encrypt_payload(const std::string& msg, const std::vector<uint8_t>& r_pub, const std::vector<uint8_t>& s_sec) {
        EncryptedPacket p{.nonce = std::vector<uint8_t>(crypto_box_NONCEBYTES), .ciphertext = std::vector<uint8_t>(msg.size() + crypto_box_MACBYTES)};
        randombytes_buf(p.nonce.data(), p.nonce.size());
        crypto_box_easy(p.ciphertext.data(), reinterpret_cast<const uint8_t*>(msg.data()), msg.size(), p.nonce.data(), r_pub.data(), s_sec.data());
        return p;
    }

    std::string decrypt_payload(const EncryptedPacket& p, const std::vector<uint8_t>& s_pub, const std::vector<uint8_t>& r_sec) {
        if (p.ciphertext.size() < crypto_box_MACBYTES) return "[ERROR]";
        std::vector<uint8_t> dec(p.ciphertext.size() - crypto_box_MACBYTES);
        if (crypto_box_open_easy(dec.data(), p.ciphertext.data(), p.ciphertext.size(), p.nonce.data(), s_pub.data(), r_sec.data()) != 0) return "[DECRYPT ERROR]";
        return std::string(dec.begin(), dec.end());
    }

    void zeroize_identity(IdentityBundle& id) {
        sodium_memzero(id.ed_secret_key.data(), id.ed_secret_key.size());
        sodium_memzero(id.x_secret_key.data(), id.x_secret_key.size());
    }
}
