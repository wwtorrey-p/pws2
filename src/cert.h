#pragma once
#include <string>

// Returns a self-signed ECDSA cert/key pair, generating and persisting one on first boot.
bool ensureCert(std::string& certPem, std::string& keyPem);
