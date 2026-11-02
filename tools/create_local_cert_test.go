package main

import (
	"crypto/ecdsa"
	"crypto/x509"
	"net"
	"path/filepath"
	"testing"
)

func TestServerCertificateIsValidForConfiguredIP(t *testing.T) {
	dir := t.TempDir()
	ca, caKey, err := createCA(filepath.Join(dir, "ca.crt"), filepath.Join(dir, "ca.key"))
	if err != nil {
		t.Fatal(err)
	}

	ip := net.ParseIP("192.168.1.73")
	certDER, keyDER, err := createServerCertificate(ca, caKey, ip)
	if err != nil {
		t.Fatal(err)
	}
	cert, err := x509.ParseCertificate(certDER)
	if err != nil {
		t.Fatal(err)
	}
	key, err := x509.ParseECPrivateKey(keyDER)
	if err != nil {
		t.Fatal(err)
	}
	publicKey, ok := cert.PublicKey.(*ecdsa.PublicKey)
	if !ok || !publicKey.Equal(&key.PublicKey) {
		t.Fatal("server certificate and private key do not match")
	}

	roots := x509.NewCertPool()
	roots.AddCert(ca)
	if _, err := cert.Verify(x509.VerifyOptions{
		Roots: roots, DNSName: ip.String(), KeyUsages: []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
	}); err != nil {
		t.Fatalf("server certificate did not verify for %s: %v", ip, err)
	}
	if _, err := cert.Verify(x509.VerifyOptions{
		Roots: roots, DNSName: "192.168.1.74", KeyUsages: []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
	}); err == nil {
		t.Fatal("server certificate unexpectedly verified for a different IP")
	}

}
