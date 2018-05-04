#include <string.h>
#include <stdio.h>
#include <openssl/x509.h>
#include "crypto.h"
#include "common.h"
#include "agent_wget.h"
#include "iasrequest.h"
#include "logfile.h"
#include "httpparser/response.h"
#include "base64.h"
#include "hexutil.h"

using namespace std;
using namespace httpparser;

#include <string>
#include <exception>

extern char verbose;
extern char debug;

static string ias_servers[2]= {
    IAS_SERVER_DEVELOPMENT_HOST,
    IAS_SERVER_PRODUCTION_HOST
};

static string url_decode(string str);

IAS_Connection::IAS_Connection(int server_idx, uint32_t flags)
{
	c_server= ias_servers[server_idx];
	c_cert_type= "PEM";
	c_flags= flags;
	c_pwlen= 0;
	c_key_passwd= NULL;
	c_xor= NULL;
	c_server_port= IAS_PORT;
	c_proxy_mode= IAS_PROXY_AUTO;
}

IAS_Connection::~IAS_Connection()
{
	if ( c_key_passwd != NULL ) delete[] c_key_passwd;
	if ( c_xor != NULL ) delete[] c_xor;
}

int IAS_Connection::proxy(const char *server, uint16_t port)
{
	int rv= 1;
	try {
		c_proxy_server= server;
	}
	catch (int e) {
		rv= 0;
	}
	c_proxy_port= port;

	return rv;
}

int IAS_Connection::client_cert(const char *file, const char *certtype)
{
	int rv= 1;
	try {
		c_cert_file= file;
		if ( certtype != NULL ) c_cert_type= certtype;
	}
	catch (int e) {
		rv= 0;
	}
	return rv;
}

int IAS_Connection::client_key(const char *file, const char *passwd)
{
	int rv= 1;
	size_t pwlen= strlen(passwd);
	size_t i;

	try {
		c_key_file= file;
	}
	catch (int e) {
		rv= 0;
	}
	if ( ! rv ) return 0;

	if ( passwd != NULL ) {
		try {
			c_key_passwd= new unsigned char[pwlen];
			c_xor= new unsigned char[pwlen];
		}
		catch (int e) { 
			rv= 0;
		}
		if ( ! rv ) {
			if ( c_key_passwd != NULL ) delete[] c_key_passwd;
			c_key_passwd= NULL;
			return 0;
		}
	}

	//rand_bytes(c_xor, pwlen);
	for (i= 0; i< pwlen; ++i) c_key_passwd[i]= (unsigned char) passwd[i]^c_xor[i];

	return 1;
}

size_t IAS_Connection::client_key_passwd(char **passwd)
{
	size_t rv= c_pwlen;
	size_t i;

	try {
		*passwd= new char[c_pwlen+1];
	}
	catch (int e) {
		rv= 0;
	}
	if ( ! rv ) return 0;

	for (i= 0; i< c_pwlen; ++i) *passwd[i]= (char) (c_key_passwd[i] ^ c_xor[i]);
	passwd[c_pwlen]= 0;

	return rv;
}

string IAS_Connection::base_url()
{
	string url= "https://" + c_server;

	if ( c_server_port != 443 ) {
		url+= ":";
		url+= to_string(c_server_port);
	}

	url+= "/attestation/sgx/v";

	return url;
}


IAS_Request::IAS_Request(IAS_Connection *conn, uint16_t version)
{
	r_conn= conn;
	r_api_version= version;
}

IAS_Request::~IAS_Request()
{
}

int IAS_Request::sigrl(uint32_t gid, string &sigrl)
{
	Response response;
	char sgid[9];
	string url= r_conn->base_url();
	int rv;

	snprintf(sgid, 9, "%08x", gid);

	url+= to_string(r_api_version);
	url+= "/sigrl/";
	url+= sgid;

	if ( verbose ) {
		edividerWithText("IAS sigrl HTTP Request");
		eprintf("HTTP GET %s\n", url.c_str());
		edivider();
	}

	if ( http_request(this, response, url, "") ) {
		if ( verbose ) {
			edividerWithText("IAS sigrl HTTP Response");
			eputs(response.inspect().c_str());
			edivider();
		}

		if ( response.statusCode == 200 ) {
			rv= 1;
			sigrl= response.content_string();
		} else {
			rv= 0;
		}
	} else {
		rv= 0;
		eprintf("Could not query IAS\n");
	}

	return rv;
}

int IAS_Request::report(map<string,string> &payload)
{
	Response response;
	map<string,string>::iterator imap;
	string url= r_conn->base_url();
	string certchain;
	string body= "{\n";
	size_t cstart, cend, count, i;
	vector<X509 *> certvec;
	X509 **certar;
	X509 *sign_cert;
	STACK_OF(X509) *stack;
	string sigstr;
	size_t sigsz;
	int rv;
	unsigned char *sig;
	EVP_PKEY *pkey;
	
	for (imap= payload.begin(); imap!= payload.end(); ++imap) {
		if ( imap != payload.begin() ) {
			body.append(",\n");
		}
		body.append("\"");
		body.append(imap->first);
		body.append("\":\"");
		body.append(imap->second);
		body.append("\"");
	}
	body.append("\n}");

	url+= to_string(r_api_version);
	url+= "/report";

	if ( verbose ) {
		edividerWithText("IAS report HTTP Request");
		eprintf("HTTP POST %s\n", url.c_str());
		edivider();
	}

	if ( http_request(this, response, url, body) ) {
		if ( verbose ) {
			edividerWithText("IAS report HTTP Response");
			eputs(response.inspect().c_str());
			edivider();
		}
	} else {
		eprintf("Could not query IAS\n");
		return 0;
	}

	/*
	 * The response body has the attestation report. The headers have
	 * a signature of the report, and the public signing certificate.
	 * We need to:
	 *
	 * 1) Verify the certificate chain, to ensure it's issued by the
	 *    Intel CA (passed with the -A option).
	 *
	 * 2) Extract the public key from the signing cert, and verify
	 *    the signature.
	 */

	// Get the certificate chain from the headers 

	certchain= response.headers_as_string("X-IASReport-Signing-Certificate");
	if ( certchain == "" ) {
		eprintf("Header X-IASReport-Signing-Certificate not found\n");
		return 0;
	}

	// URL decode
	try {
		certchain= url_decode(certchain);
	}
	catch (int e) {
		eprintf("invalid URL encoding in header X-IASReport-Signing-Certificate\n");
		return 0;
	}

	// Build the cert stack. Find the positions in the string where we
	// have a BEGIN block.

	cstart= cend= 0;
	while (cend != string::npos ) {
		X509 *cert;
		size_t len;

		cend= certchain.find("-----BEGIN", cstart+1);
		len= ( (cend == string::npos) ? certchain.length() : cend )-cstart;

		if ( verbose ) {
			edividerWithText("Certficate");
			eputs(certchain.substr(cstart, len).c_str());
			eprintf("\n");
			edivider();
		}

		if ( ! cert_load(&cert, certchain.substr(cstart, len).c_str()) ) {
			crypto_perror("cert_load");
			return 0;
		}

		certvec.push_back(cert);
		cstart= cend;
	}

	count= certvec.size();
	if ( debug ) eprintf( "+++ Found %lu certificates in chain\n", count);

	certar= (X509**) malloc(sizeof(X509 *)*(count+1));
	if ( certar == 0 ) {
		perror("malloc");
		return 0;
	}
	for (i= 0; i< count; ++i) certar[i]= certvec[i];
	certar[count]= NULL;

	// Create a STACK_OF(X509) stack from our certs

	stack= cert_stack_build(certar);
	if ( stack == NULL ) {
		crypto_perror("cert_stack_build");
		return 0;
	}

	// Now verify the signing certificate

	rv= cert_verify(this->conn()->cert_store(), stack);

	if ( ! rv ) {
		crypto_perror("cert_stack_build");
		eprintf("certificate verification failure\n");
		goto cleanup;
	} else eprintf("+++ certificate chain verified\n", rv);

	// The signing cert is valid, so extract and verify the signature

	sigstr= response.headers_as_string("X-IASReport-Signature");
	if ( sigstr == "" ) {
		eprintf("Header X-IASReport-Signature not found\n");
		rv= 0;
		goto cleanup;
	}

	sig= (unsigned char *) base64_decode(sigstr.c_str(), &sigsz);
	if ( sig == NULL ) {
		eprintf("Could not decode signature\n");
		goto cleanup;
	}

	if ( verbose ) {
		edividerWithText("Report Signature");
		print_hexstring(stderr, sig, sigsz);
		if ( fplog != NULL ) print_hexstring(fplog, sig, sigsz);
		eprintf( "\n");
		edivider();
	}

	sign_cert= certvec[0]; /* The first cert in the list */

	/*
	 * The report body is SHA256 signed with the private key of the
	 * signing cert.  Extract the public key from the certificate and
	 * verify the signature.
	 */

	if ( debug ) eprintf("+++ Extracting public key from signing cert\n");
	pkey= X509_get_pubkey(sign_cert);
	if ( pkey == NULL ) {
		eprintf("Could not extract public key from certificate\n");
		free(sig);
		goto cleanup;
	}

	if ( debug ) {
		eprintf("+++ Verifying signature over report body\n");
		edividerWithText("Report");
		eputs(response.content_string().c_str());
		eprintf("\n");
		edivider();
		eprintf("Content-length: %lu bytes\n", response.content_string().length());
		edivider();
	}

	if ( ! sha256_verify(
		(const unsigned char *) response.content_string().c_str(),
		response.content_string().length(), sig, sigsz, pkey, &rv) ) {

		free(sig);
		crypto_perror("sha256_verify");
		eprintf("Could not validate signature\n");
	} else {
		if ( rv ) eprintf("+++ Signature verified\n");
		else eprintf("Invalid report signature\n");
	}

cleanup:
	if ( pkey != NULL ) EVP_PKEY_free(pkey);
	cert_stack_free(stack);
	free(certar);
	for (i= 0; i<count; ++i) X509_free(certvec[i]);
	free(sig);

	return rv;
}

// A simple URL decoder 

static string url_decode(string str)
{
	string decoded;
	size_t i;
	size_t len= str.length();

	for (i= 0; i< len; ++i) {
		if ( str[i] == '+' ) decoded+= ' ';
		else if ( str[i] == '%' ) {
			char *e= NULL;
			unsigned long int v;

			// Have a % but run out of characters in the string

			if ( i+3 > len ) throw std::length_error("premature end of string");

			v= strtoul(str.substr(i+1, 2).c_str(), &e, 16);

			// Have %hh but hh is not a valid hex code.
			if ( *e ) throw std::out_of_range("invalid encoding");

			decoded+= static_cast<char>(v);
			i+= 2;
		} else decoded+= str[i];
	}

	return decoded;
}

