// Command create_local_cert creates a local CA and a server certificate for the station.
package main

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/pem"
	"flag"
	"fmt"
	"math/big"
	"net"
	"os"
	"path/filepath"
	"time"
)

func main() {
	ipText := flag.String("ip", "", "station IPv4 or IPv6 address")
	flag.Parse()
	ip := net.ParseIP(*ipText)
	if ip == nil {
		fatalf("provide a valid station IP with -ip")
	}

	configDir, err := os.UserConfigDir()
	if err != nil {
		fatalf("locate user config directory: %v", err)
	}
	caDir := filepath.Join(configDir, "weather-station", "local-ca")
	if err := os.MkdirAll(caDir, 0700); err != nil {
		fatalf("create local CA directory: %v", err)
	}

	caCert, caKey, err := loadOrCreateCA(caDir)
	if err != nil {
		fatalf("prepare local CA: %v", err)
	}
	serverCert, serverKey, err := createServerCertificate(caCert, caKey, ip)
	if err != nil {
		fatalf("create server certificate: %v", err)
	}

	certDir := "certs"
	if err := os.MkdirAll(certDir, 0700); err != nil {
		fatalf("create project certs directory: %v", err)
	}
	if err := writePEM(filepath.Join(certDir, "local-ca.crt"), "CERTIFICATE", caCert.Raw); err != nil {
		fatalf("write local CA certificate: %v", err)
	}
	if err := writePEM(filepath.Join(certDir, "server.crt"), "CERTIFICATE", serverCert); err != nil {
		fatalf("write server certificate: %v", err)
	}
	if err := writePEM(filepath.Join(certDir, "server.key"), "EC PRIVATE KEY", serverKey); err != nil {
		fatalf("write server private key: %v", err)
	}
	fmt.Printf("Created server certificate for %s in %s\n", ip, certDir)
}

func loadOrCreateCA(dir string) (*x509.Certificate, *ecdsa.PrivateKey, error) {
	certPath := filepath.Join(dir, "ca.crt")
	keyPath := filepath.Join(dir, "ca.key")
	certData, certErr := os.ReadFile(certPath)
	keyData, keyErr := os.ReadFile(keyPath)
	if os.IsNotExist(certErr) && os.IsNotExist(keyErr) {
		return createCA(certPath, keyPath)
	}
	if certErr != nil {
		return nil, nil, certErr
	}
	if keyErr != nil {
		return nil, nil, keyErr
	}

	certBlock, _ := pem.Decode(certData)
	keyBlock, _ := pem.Decode(keyData)
	if certBlock == nil || keyBlock == nil {
		return nil, nil, fmt.Errorf("invalid CA PEM files in %s", dir)
	}
	cert, err := x509.ParseCertificate(certBlock.Bytes)
	if err != nil {
		return nil, nil, err
	}
	key, err := x509.ParseECPrivateKey(keyBlock.Bytes)
	if err != nil {
		return nil, nil, err
	}
	publicKey, ok := cert.PublicKey.(*ecdsa.PublicKey)
	if !ok || !cert.IsCA || cert.CheckSignatureFrom(cert) != nil || !publicKey.Equal(&key.PublicKey) {
		return nil, nil, fmt.Errorf("CA certificate and key are invalid or do not match")
	}
	if time.Now().After(cert.NotAfter) {
		return nil, nil, fmt.Errorf("local CA expired; remove %s and recreate it", dir)
	}
	return cert, key, nil
}

func createCA(certPath, keyPath string) (*x509.Certificate, *ecdsa.PrivateKey, error) {
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		return nil, nil, err
	}
	serial, err := serialNumber()
	if err != nil {
		return nil, nil, err
	}
	now := time.Now()
	template := &x509.Certificate{
		SerialNumber:          serial,
		Subject:               pkix.Name{CommonName: "Weather Station Local CA"},
		NotBefore:             now.Add(-5 * time.Minute),
		NotAfter:              now.AddDate(20, 0, 0),
		KeyUsage:              x509.KeyUsageCertSign | x509.KeyUsageCRLSign,
		BasicConstraintsValid: true,
		IsCA:                  true,
		MaxPathLenZero:        true,
	}
	der, err := x509.CreateCertificate(rand.Reader, template, template, &key.PublicKey, key)
	if err != nil {
		return nil, nil, err
	}
	if err := writePEM(certPath, "CERTIFICATE", der); err != nil {
		return nil, nil, err
	}
	keyDER, err := x509.MarshalECPrivateKey(key)
	if err != nil {
		return nil, nil, err
	}
	if err := writePEM(keyPath, "EC PRIVATE KEY", keyDER); err != nil {
		return nil, nil, err
	}
	cert, err := x509.ParseCertificate(der)
	return cert, key, err
}

func createServerCertificate(ca *x509.Certificate, caKey *ecdsa.PrivateKey, ip net.IP) ([]byte, []byte, error) {
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		return nil, nil, err
	}
	serial, err := serialNumber()
	if err != nil {
		return nil, nil, err
	}
	now := time.Now()
	template := &x509.Certificate{
		SerialNumber:          serial,
		Subject:               pkix.Name{CommonName: "weather-station"},
		NotBefore:             now.Add(-5 * time.Minute),
		NotAfter:              now.AddDate(10, 0, 0),
		KeyUsage:              x509.KeyUsageDigitalSignature,
		ExtKeyUsage:           []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
		BasicConstraintsValid: true,
		IPAddresses:           []net.IP{ip},
	}
	der, err := x509.CreateCertificate(rand.Reader, template, ca, &key.PublicKey, caKey)
	if err != nil {
		return nil, nil, err
	}
	keyDER, err := x509.MarshalECPrivateKey(key)
	return der, keyDER, err
}

func serialNumber() (*big.Int, error) {
	return rand.Int(rand.Reader, new(big.Int).Lsh(big.NewInt(1), 128))
}

func writePEM(path, kind string, data []byte) error {
	file, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, 0600)
	if err != nil {
		return err
	}
	if err := pem.Encode(file, &pem.Block{Type: kind, Bytes: data}); err != nil {
		_ = file.Close()
		return err
	}
	return file.Close()
}

func fatalf(format string, args ...any) {
	fmt.Fprintf(os.Stderr, format+"\n", args...)
	os.Exit(1)
}
