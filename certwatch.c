/*
   Copyright 2005-2019 Red Hat, Inc.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
  
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
  
   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
   
   In addition, as a special exception, Red Hat, Inc. gives permission
   to link the code of this program with the OpenSSL library (or with
   modified versions of OpenSSL that use the same license as OpenSSL),
   and distribute linked combinations including the two. You must obey
   the GNU General Public License in all respects for all of the code
   used other than OpenSSL. If you modify this file, you may extend
   this exception to your version of the file, but you are not
   obligated to do so. If you do not wish to do so, delete this
   exception statement from your version.

*/

/* Certificate expiry warning generation code, based on code from
 * Stronghold - Joe Orton. */

#include <openssl/x509.h>
#include <openssl/pem.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <getopt.h>
#include <time.h>

static int warn_period = 30;
static const char *warn_address = "root";

/* Turn an ASN.1 UTCTIME object into a time_t. */
static time_t decode_utctime(const ASN1_UTCTIME *utc)
{
    struct tm tm = {0};
    int i = utc->length;

    if (i < 10)
	return -1;
    for (i = 0; i < 10; i++)
	if ((utc->data[i] > '9') || (utc->data[i] < '0'))
	    return -1;

    tm.tm_year = (utc->data[0]-'0') * 10 + (utc->data[1]-'0');

    /* Deal with Year 2000 like eay did */
    if (tm.tm_year < 70)
	tm.tm_year += 100;

    tm.tm_mon = (utc->data[2]-'0') * 10 + (utc->data[3]-'0') - 1;
    tm.tm_mday = (utc->data[4]-'0') * 10 + (utc->data[5]-'0');
    tm.tm_hour = (utc->data[6]-'0') * 10 + (utc->data[7]-'0');
    tm.tm_min = (utc->data[8]-'0') * 10 + (utc->data[9]-'0');
    tm.tm_sec = (utc->data[10]-'0') * 10 + (utc->data[11]-'0');

    return mktime(&tm) - timezone;
}

/* Print a warning message that the certificate in 'filename', issued
 * to hostname 'hostname', will expire (or has expired). */
static int warning(FILE *out, const char *filename, const char *hostname,
                   time_t start, time_t end, time_t now, int quiet)
{
    int renew = 1, days = (end - now) / (3600 * 24);    /* days till expiry */
    char subj[50];

    if (start > now) {
        strcpy(subj, "is not yet valid");
        renew = 0;
    }
    else if (days < 0) {
        strcpy(subj, "has expired");
    }
    else if (days == 0) {
        strcpy(subj, "will expire today");
    }
    else if (days == 1) {
        sprintf(subj, "will expire tomorrow");
    }
    else if (days < warn_period) {
        sprintf(subj, "will expire in %d days", days);
    }
    else {
        return 0; /* nothing to warn about. */
    }

    if (quiet) return 1;

    fprintf(out, "To: %s\n", warn_address);
    fprintf(out, "Subject: The certificate for %s %s\n", hostname, subj);
    fputs("\n", out);
    
    fprintf(out, 
            " ################# SSL/TLS Certificate Warning ################\n\n");

    fprintf(out, 
            "  Certificate for hostname '%s', in file:\n\n"
            "     %s\n\n",
            hostname, filename);

    if (renew) {
        fputs("  The certificate needs to be renewed.  Web browsers and \n"
              "  other clients will not be able to correctly connect to this\n"
              "  web site using SSL/TLS until the certificate is renewed.\n",
              out);
    }
    else {
        char until[30] = "(unknown date)";
        ctime_r(&start, until);
        if (strlen(until) > 2) until[strlen(until)-1] = '\0';
        fprintf(out,
                "  The certificate is not valid until %s.\n\n"
                "  Web browsers and other clients will not be able to correctly "
                "  connect to this web site using SSL/TLS until the certificate "
                "  becomes valid.\n",                
                until);
    }

    fputs("\n"
          " ##############################################################\n"
          "                                      Generated by certwatch(1)\n\n",
          out);
    return 1;
}

/* Extract the common name of 'cert' into 'buf'. */
static int get_common_name(X509 *cert, char *buf, size_t bufsiz)
{
    X509_NAME *name = X509_get_subject_name(cert);
    
    if (!name) return -1;

    return X509_NAME_get_text_by_NID(name, NID_commonName, buf, bufsiz) == -1;
}

/* Check whether the certificate in filename 'filename' has expired;
 * issue a warning message if 'quiet' is zero.  If quiet is non-zero,
 * returns one to indicate that a warning would have been issued, zero
 * to indicate no warning would be issued, or -1 if an error
 * occurred. */
static int check_cert(const char *filename, int quiet)
{
    X509 *cert;
    FILE *fp;
    ASN1_UTCTIME *notAfter, *notBefore;
    time_t begin, end, now;
    char cname[128];

    /* parse the cert */
    if ((fp = fopen(filename, "r")) == NULL) return -1;
    cert = PEM_read_X509(fp, NULL, NULL, NULL);
    fclose(fp);
    if (cert == NULL) return -1;
    
    /* determine the validity period of the cert. */
    notAfter = X509_get_notAfter(cert);
    notBefore = X509_get_notBefore(cert);

    /* get time_t's out of X509 times */
    begin = decode_utctime(notBefore);
    end = decode_utctime(notAfter);
    now = time(NULL);
    if (end == -1 || begin == -1 || now == -1) return -1;
    
    /* find the subject's commonName attribute */
    if (get_common_name(cert, cname, sizeof cname))
        return -1;

    X509_free(cert);

    /* don't warn about the automatically generate certificate */
    if (strcmp(cname, "localhost") == 0
        || strcmp(cname, "localhost.localdomain") == 0)
        return -1;

    return warning(stdout, filename, cname, begin, end, now, quiet);
}

static void usage(FILE *out)
{
    fprintf(out, "Usage: certwatch [options...] <certificate>\n");
    fprintf(out, "  -a, --address <addr> Recipient address [root]\n");
    fprintf(out, "  -p, --period <days>  Number of days before expiry [30]\n");
    fprintf(out, "  -q, --quiet          Enable quiet mode\n");
    fprintf(out, "  -h, --help           Display usage instructinos.\n");
}

int main(int argc, char **argv)
{
    int optc, quiet = 0;
    static const struct option options[] = {
        { "quiet", no_argument, NULL, 'q' },
        { "period", required_argument, NULL, 'p' },
        { "address", required_argument, NULL, 'a' },
        { "help", no_argument, NULL, 'h' },
        { NULL }
    };

    /* The 'timezone' global is needed to adjust local times from
     * mktime() back to UTC: */
    tzset();
    
    while ((optc = getopt_long(argc, argv, "qp:a:", options, NULL)) != -1) {
        switch (optc) {
        case 'q':
            quiet = 1;
            break;
        case 'p':
            warn_period = atoi(optarg);
            break;
        case 'a':
            warn_address = strdup(optarg);
            break;
        case 'h':
            usage(stdout);
            exit(0);
        default:
            exit(2);
            break;
        }
    }

    return check_cert(argv[optind], quiet) == 1 ? EXIT_SUCCESS : EXIT_FAILURE;
}
