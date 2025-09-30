/*
 * Certificate Format Checker Tool
 * 
 * This is a utility to validate the certificate file format.
 * Compile and run it separately if you're having SSL connection issues.
 */

#include <Arduino.h>
#include <FS.h>
#include <SPIFFS.h>

// Function to check certificate validity
bool validateCertificate(const char* cert) {
    // Check for key PEM markers
    if (strstr(cert, "-----BEGIN CERTIFICATE-----") == NULL) {
        Serial.println("ERROR: Certificate is missing BEGIN marker");
        return false;
    }
    
    if (strstr(cert, "-----END CERTIFICATE-----") == NULL) {
        Serial.println("ERROR: Certificate is missing END marker");
        return false;
    }
    
    // Check for invalid characters
    const char* ptr = cert;
    while (*ptr) {
        // PEM certificates should only have printable ASCII and newlines
        if (*ptr < 32 && *ptr != '\n' && *ptr != '\r') {
            Serial.printf("ERROR: Certificate contains invalid character at position %d: %02X\n", 
                         (int)(ptr - cert), (uint8_t)*ptr);
            return false;
        }
        ptr++;
    }
    
    // Check line lengths (PEM lines should be ~64 chars)
    int lineLength = 0;
    bool inCertData = false;
    ptr = cert;
    
    while (*ptr) {
        if (*ptr == '\n' || *ptr == '\r') {
            if (inCertData && lineLength > 0 && lineLength != 64 && lineLength < 60) {
                Serial.printf("WARNING: Unusual line length %d (expected ~64 chars)\n", lineLength);
            }
            lineLength = 0;
        } else {
            lineLength++;
        }
        
        // Track when we're in the actual base64 data portion
        if (strncmp(ptr, "-----BEGIN CERTIFICATE-----", 27) == 0) {
            inCertData = true;
        } else if (strncmp(ptr, "-----END CERTIFICATE-----", 25) == 0) {
            inCertData = false;
        }
        
        ptr++;
    }
    
    return true;
}

// Function to fix common certificate issues
String fixCertificate(const String& cert) {
    String fixed = cert;
    
    // Ensure the certificate has proper begin/end markers
    if (fixed.indexOf("-----BEGIN CERTIFICATE-----") == -1) {
        fixed = "-----BEGIN CERTIFICATE-----\n" + fixed;
    }
    
    if (fixed.indexOf("-----END CERTIFICATE-----") == -1) {
        fixed += "\n-----END CERTIFICATE-----\n";
    }
    
    // Replace any weird whitespace with standard newlines
    fixed.replace("\r\n", "\n");
    fixed.replace("\r", "\n");
    
    // Ensure there's no extra whitespace at the beginning/end
    fixed.trim();
    
    return fixed;
}

// This can be included in your main sketch or compiled separately
void checkCertificate() {
    // Initialize SPIFFS
    if (!SPIFFS.begin(true)) {
        Serial.println("Failed to mount SPIFFS");
        return;
    }
    
    // Try to open the certificate file
    File certFile = SPIFFS.open("/zello-io.crt", "r");
    if (!certFile) {
        Serial.println("Failed to open certificate file!");
        return;
    }
    
    // Read the certificate
    String cert = certFile.readString();
    certFile.close();
    
    // Print certificate info
    Serial.println("\n=== Certificate Information ===");
    Serial.printf("Certificate size: %d bytes\n", cert.length());
    Serial.println("First 64 characters:");
    Serial.println(cert.substring(0, 64));
    
    // Validate the certificate
    Serial.println("\n=== Certificate Validation ===");
    bool isValid = validateCertificate(cert.c_str());
    
    if (isValid) {
        Serial.println("Certificate format appears to be valid!");
    } else {
        Serial.println("Certificate has format issues. Attempting to fix...");
        String fixedCert = fixCertificate(cert);
        
        // Validate the fixed certificate
        if (validateCertificate(fixedCert.c_str())) {
            Serial.println("Fixed certificate appears to be valid!");
            Serial.println("Writing fixed certificate to SPIFFS...");
            
            // Write the fixed certificate back to SPIFFS
            File fixedFile = SPIFFS.open("/zello-io.crt", "w");
            if (fixedFile) {
                fixedFile.print(fixedCert);
                fixedFile.close();
                Serial.println("Fixed certificate saved successfully!");
            } else {
                Serial.println("Failed to write fixed certificate!");
            }
        } else {
            Serial.println("Certificate still has issues after fixing attempt.");
        }
    }
}
