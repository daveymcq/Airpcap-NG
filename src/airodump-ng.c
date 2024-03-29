/*
 *  pcap-compatible 802.11 packet sniffer (Win32 version)
 *
 *  Copyright (C) 2004,2005  Christophe Devine
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#define _CRT_SECURE_NO_DEPRECATE

#include <windows.h>
#include <stdio.h>
#include <time.h>

#define snprintf _snprintf

#include "capture.h"
#include "console.h"
#include "timeval.h"
#include "pcap.h"
#include "version.h"

#include "uniqueiv.c"

#define FORMAT_CAP 1
#define FORMAT_IVS 2

#define REFRESH_TIMEOUT 200000

#define BROADCAST_ADDR "\xFF\xFF\xFF\xFF\xFF\xFF"

int abg_chans [] =
{
    1, 7, 13, 2, 8, 3, 14, 9, 4, 10, 5, 11, 6, 12,
    36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108,
    112, 116, 120, 124, 128, 132, 136, 140, 149,
    153, 157, 161, 184, 188, 192, 196, 200,0
};

int bg_chans  [] =
{
    1, 7, 13, 2, 8, 3, 14, 9, 4, 10, 5, 11, 6, 12, 0
};

int a_chans   [] =
{
    36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108,
    112, 116, 120, 124, 128, 132, 136, 140, 149,
    153, 157, 161, 184, 188, 192, 196, 200, 0
};

/* linked list of detected access points */

struct AP_info
{
    struct AP_info *prev;     /* the prev AP in list      */
    struct AP_info *next;     /* the next AP in list      */

    time_t tinit, tlast;      /* first and last time seen */

    int power, chanl;         /* signal power and channel */
    int speed, crypt;         /* maxrate & encryption alg */

    unsigned long nb_bcn;     /* total number of beacons  */
    unsigned long nb_pkt;     /* total number of packets  */
    unsigned long nb_data;    /* number of WEP data pkts  */

    unsigned char bssid[6];   /* the access point's MAC   */
    unsigned char essid[33];  /* ascii network identifier */

    unsigned char lanip[4];   /* last detected ip address */
                              /* if non-encrypted network */

    unsigned char **uiv_root; /* unique iv root structure */
                              /* if wep-encrypted network */
};

/* linked list of detected clients */

struct ST_info
{
    struct ST_info *prev;    /* the prev client in list   */
    struct ST_info *next;    /* the next client in list   */
    struct AP_info *base;    /* AP this client belongs to */
    time_t tinit, tlast;     /* first and last time seen  */
    int power;               /* signal power              */
    unsigned long nb_pkt;    /* total number of packets   */
    unsigned char stmac[6];  /* the client's MAC address  */
};

/* bunch of global stuff */

struct AP_info *ap_1st, *ap_end;
struct AP_info *ap_cur, *ap_prv;

struct ST_info *st_1st, *st_end;
struct ST_info *st_cur, *st_prv;

struct pcap_file_header pfh_out;
struct pcap_file_header pfh_out;

unsigned char prev_bssid[6];

FILE *f_cap_in  = NULL;
FILE *f_csv_out = NULL;
FILE *f_cap_out = NULL;
FILE *f_ivs_out = NULL;

const unsigned char llcnull [4]= {0, 0, 0, 0 };

int dump_initialize( char *output_prefix, int ivs_only )
{
    int n;
    char o_filename[1024];

    ap_1st = ap_end = NULL;
    st_1st = st_end = NULL;

    /* create the output csv file */

    if( strlen( output_prefix ) >= sizeof( o_filename ) - 5 )
        output_prefix[sizeof( o_filename ) - 5] = '\0';

    if( strcmp( output_prefix, "-" ) != 0 )
    {
        memset( o_filename, 0, sizeof( o_filename ) );
        snprintf( o_filename,  sizeof( o_filename ) - 1,
                  "%s.txt", output_prefix );

        if( ( f_csv_out = fopen( o_filename, "wb+" ) ) == NULL )
        {
            perror( "fopen failed" );
            fprintf( stderr, "\n  Could not create \"%s\".\n", o_filename );
            return( 1 );
        }
    }

    /* open or create the output packet capture file */

    if( ivs_only == 0 )
    {
        n = sizeof( struct pcap_file_header );

        if( strcmp( output_prefix, "-" ) != 0 )
        {
            memset( o_filename, 0, sizeof( o_filename ) );
            snprintf( o_filename,  sizeof( o_filename ) - 1,
                      "%s.cap", output_prefix );
        }
        else
        {
            f_cap_out = _fdopen( 1, "wb" );
            goto write_cap_header;
        }

        if( ( f_cap_out = fopen( o_filename, "rb+" ) ) == NULL )
        {
        create_cap_file:

            if( ( f_cap_out = fopen( o_filename, "wb+" ) ) == NULL )
            {
                perror( "fopen failed" );
                fprintf( stderr, "\n  Could not create \"%s\".\n", o_filename );
                return( 1 );
            }

        write_cap_header:

            pfh_out.magic           = TCPDUMP_MAGIC;
            pfh_out.version_major   = PCAP_VERSION_MAJOR;
            pfh_out.version_minor   = PCAP_VERSION_MINOR;
            pfh_out.thiszone        = 0;
            pfh_out.sigfigs         = 0;
            pfh_out.snaplen         = 65535;
            pfh_out.linktype        = LINKTYPE_IEEE802_11;

            if( fwrite( &pfh_out, 1, n, f_cap_out ) != (size_t) n )
            {
                perror( "fwrite(pcap file header) failed" );
                return( 1 );
            }
        }
        else
        {
            if( fread( &pfh_out, 1, n, f_cap_out ) != (size_t) n )
                goto create_cap_file;

            if( pfh_out.magic != TCPDUMP_MAGIC &&
                pfh_out.magic != TCPDUMP_CIGAM )
            {
                fprintf( stderr, "\n  \"%s\" isn't a pcap file (expected "
                                 "TCPDUMP_MAGIC).\n", o_filename );
                return( 1 );
            }

            if( pfh_out.magic == TCPDUMP_CIGAM )
                SWAP32( pfh_out.linktype );

            if( pfh_out.linktype != LINKTYPE_IEEE802_11 )
            {
                fprintf( stderr, "\n  Wrong linktype from pcap file header "
                                 "(expected LINKTYPE_IEEE802_11) -\n"
                                 "this doesn't look like a regular 802.11 "
                                 "capture.\n" );
                return( 1 );
            }

            if( fseek( f_cap_out, 0, SEEK_END ) != 0 )
            {
                perror( "fseek(SEEK_END) failed" );
                return( 1 );
            }
        }
    }

    if( ivs_only == 1 )
    {
        memset( prev_bssid, 0, 6 );

        if( strcmp( output_prefix, "-" ) != 0 )
        {
            memset( o_filename, 0, sizeof( o_filename ) );
            snprintf( o_filename,  sizeof( o_filename ) - 1,
                      "%s.ivs", output_prefix );
        }
        else
        {
            f_ivs_out = _fdopen( 1, "wb" );
            goto write_ivs_header;
        }

        if( ( f_ivs_out = fopen( o_filename, "rb+" ) ) == NULL )
        {
        create_ivs_file:

            if( ( f_ivs_out = fopen( o_filename, "wb+" ) ) == NULL )
            {
                perror( "fopen failed" );
                fprintf( stderr, "\n  Could not create \"%s\".\n", o_filename );
                return( 1 );
            }

        write_ivs_header:

            if( fwrite( IVSONLY_MAGIC, 1, 4, f_ivs_out ) != sizeof( n ) )
            {
                perror( "fwrite(IVs file header) failed" );
                return( 1 );
            }
        }
        else
        {
            unsigned char ivs_hdr[4];

            if( fread( ivs_hdr, 1, 4, f_ivs_out ) != 4 )
                goto create_ivs_file;

            if( memcmp( ivs_hdr, IVSONLY_MAGIC, 4 ) != 0 )
            {
                fprintf( stderr, "\n  \"%s\" isn't a IVs file (expected "
                                 "IVSONLY_MAGIC).\n", o_filename );
                return( 1 );
            }

            if( fseek( f_ivs_out, 0, SEEK_END ) != 0 )
            {
                perror( "fseek(SEEK_END) failed" );
                return( 1 );
            }
        }
    }

    return( 0 );
}

int dump_add_packet( unsigned char *h80211, int caplen, int power,
                     int channel, uint tv_sec, uint tv_usec )

{
    int i, n;

    struct pcap_pkthdr pkh;

    unsigned char *p;
    unsigned char bssid[6];
    unsigned char stmac[6];

    ap_cur = NULL;
    st_cur = NULL;

    pkh.caplen = pkh.len = caplen;

    /* skip packets smaller than a 802.11 header */

    if( pkh.caplen < 24 )
        goto write_packet;

    /* skip (uninteresting) control frames */

    if( ( h80211[0] & 0x0C ) == 0x04 )
        goto write_packet;

	/* if it's a LLC null packet, just forget it (may change in the future) */

	if ( caplen > 28)
		if ( memcmp(h80211 + 24, llcnull, 4) == 0)
			return ( 0 );


    /* locate the access point's MAC address */

    switch( h80211[1] & 3 )
    {
        case  0: memcpy( bssid, h80211 + 16, 6 ); break;
        case  1: memcpy( bssid, h80211 +  4, 6 ); break;
        case  2: memcpy( bssid, h80211 + 10, 6 ); break;
        default: memcpy( bssid, h80211 +  4, 6 ); break;
    }

    /* skip broadcast packets */

    if( memcmp( bssid, BROADCAST_ADDR, 6 ) == 0 )
        goto write_packet;

    /* update our chained list of access points */

    ap_cur = ap_1st;
    ap_prv = NULL;

    while( ap_cur != NULL )
    {
        if( ! memcmp( ap_cur->bssid, bssid, 6 ) )
            break;

        ap_prv = ap_cur;
        ap_cur = ap_cur->next;
    }

    /* if it's a new access point, add it */

    if( ap_cur == NULL )
    {
        if( ! ( ap_cur = (struct AP_info *) malloc(
                         sizeof( struct AP_info ) ) ) )
        {
            perror( "malloc failed" );
            return( 1 );
        }

        memset( ap_cur, 0, sizeof( struct AP_info ) );

        if( ap_1st == NULL )
            ap_1st = ap_cur;
        else
            ap_prv->next  = ap_cur;

        memcpy( ap_cur->bssid, bssid, 6 );

        ap_cur->prev = ap_prv;

        if( tv_sec == 0 )
        {
            ap_cur->tinit = time( NULL );
            ap_cur->tlast = time( NULL );
        }
        else
        {
            ap_cur->tinit = tv_sec;
            ap_cur->tlast = tv_sec;
        }

        ap_cur->power = power;

        ap_cur->chanl = -1;
        ap_cur->speed = -1;
        ap_cur->crypt = -1;

        ap_cur->uiv_root = uniqueiv_init();

        ap_end = ap_cur;
    }

    if( tv_sec == 0 )
        ap_cur->tlast = time( NULL );
    else
        ap_cur->tlast = tv_sec;

    if( ( h80211[1] & 1 ) == 0 )
        ap_cur->power = power;

    if( h80211[0] == 0x80 )
        ap_cur->nb_bcn++;

    ap_cur->nb_pkt++;

    /* locate the station MAC in the 802.11 header */

    switch( h80211[1] & 3 )
    {
        case  0: memcpy( stmac, h80211 + 10, 6 ); break;
        case  1: memcpy( stmac, h80211 + 10, 6 ); break;
        case  2:

            /* reject broadcast MACs */

            if( h80211[4] != 0 ) goto skip_station;
            memcpy( stmac, h80211 +  4, 6 ); break;

        default: goto skip_station; break;
    }

    /* skip non-data packets */

    if( ( h80211[0] & 0x0C ) != 0x08 )
        goto skip_station;

    /* update our chained list of wireless clients */

    st_cur = st_1st;
    st_prv = NULL;

    while( st_cur != NULL )
    {
        if( ! memcmp( st_cur->stmac, stmac, 6 ) )
            break;

        st_prv = st_cur;
        st_cur = st_cur->next;
    }

    /* if it's a new client, add it */

    if( st_cur == NULL )
    {
        if( ! ( st_cur = (struct ST_info *) malloc(
                         sizeof( struct ST_info ) ) ) )
        {
            perror( "malloc failed" );
            return( 1 );
        }

        memset( st_cur, 0, sizeof( struct ST_info ) );

        if( st_1st == NULL )
            st_1st = st_cur;
        else
            st_prv->next  = st_cur;

        memcpy( st_cur->stmac, stmac, 6 );

        st_cur->prev = st_prv;
        st_cur->base = ap_cur;

        if( tv_sec == 0 )
        {
            st_cur->tinit = time( NULL );
            st_cur->tlast = time( NULL );
        }
        else
        {
            st_cur->tinit = tv_sec;
            st_cur->tlast = tv_sec;
        }

        st_cur->power = power;

        st_end = st_cur;
    }

    /* every 1s, update the last time seen & receive power */

    if( tv_sec == 0 )
        st_cur->tlast = time( NULL );
    else
        st_cur->tlast = tv_sec;

    if( ( h80211[1] & 3 ) == 1 )
        st_cur->power = power;

    st_cur->nb_pkt++;

skip_station:

    /* packet parsing: Beacon or Probe Response */

    if( h80211[0] == 0x80 ||
        h80211[0] == 0x50 )
    {
        if( ap_cur->crypt < 0 )
            ap_cur->crypt = ( h80211[34] & 0x10 ) >> 4;

        p = h80211 + 36;

        while( p < h80211 + pkh.caplen )
        {
            if( p + 2 + p[1] > h80211 + pkh.caplen )
                break;

            if( p[0] == 0x00 && p[1] > 0 && p[2] != '\0' &&
                ( p[1] > 1 || p[2] != ' ' ) )
            {
                /* found a non-cloaked ESSID */

                n = ( p[1] > 32 ) ? 32 : p[1];

                memset( ap_cur->essid, 0, 33 );
                memcpy( ap_cur->essid, p + 2, n );

                for( i = 0; i < n; i++ )
                    if( ap_cur->essid[i] < 32 ||
                      ( ap_cur->essid[i] > 126 && ap_cur->essid[i] < 160 ) )
                        ap_cur->essid[i] = '.';
            }

            if( p[0] == 0x01 || p[0] == 0x32 )
                ap_cur->speed = ( p[1 + p[1]] & 0x7F ) / 2;

            if( p[0] == 0x03 )
                ap_cur->chanl = p[2];

            p += 2 + p[1];
        }
    }

    /* packet parsing: Association Request */

    if( h80211[0] == 0x00 )
    {
        p = h80211 + 28;

        while( p < h80211 + pkh.caplen )
        {
            if( p + 2 + p[1] > h80211 + pkh.caplen )
                break;

            if( p[0] == 0x00 && p[1] > 0 && p[2] != '\0' &&
                ( p[1] > 1 || p[2] != ' ' ) )
            {
                /* found a non-cloaked ESSID */

                n = ( p[1] > 32 ) ? 32 : p[1];

                memset( ap_cur->essid, 0, 33 );
                memcpy( ap_cur->essid, p + 2, n );

                for( i = 0; i < n; i++ )
                    if( ap_cur->essid[i] < 32 ||
                      ( ap_cur->essid[i] > 126 && ap_cur->essid[i] < 160 ) )
                        ap_cur->essid[i] = '.';
            }

            p += 2 + p[1];
        }
    }

    /* packet parsing: some data */

    if( ap_cur->chanl == -1 )
        ap_cur->chanl = channel;

    if( ( h80211[0] & 0x0C ) == 0x08 )
    {
        /* check the SNAP header to see if data is encrypted */

        unsigned int z = ( ( h80211[1] & 3 ) != 3 ) ? 24 : 30;

        if( z + 26 > pkh.caplen )
            goto write_packet;

        if( h80211[z] == h80211[z + 1] && h80211[z + 2] == 0x03 )
        {
            if( ap_cur->crypt < 0 )
                ap_cur->crypt = 0;

            /* if ethertype == IPv4, find the LAN address */

            if( h80211[z + 6] == 0x08 && h80211[z + 7] == 0x00 &&
                ( h80211[1] & 3 ) == 0x01 )
                    memcpy( ap_cur->lanip, &h80211[z + 20], 4 );

            if( h80211[z + 6] == 0x08 && h80211[z + 7] == 0x06 )
                memcpy( ap_cur->lanip, &h80211[z + 22], 4 );
        }
        else
            ap_cur->crypt = 2 + ( ( h80211[z + 3] & 0x20 ) >> 5 );

        if( z + 10 > pkh.caplen )
            goto write_packet;

        if( ap_cur->crypt == 2 )
        {
            /* WEP: check if we've already seen this IV */

            if( ! uniqueiv_check( ap_cur->uiv_root, &h80211[z] ) )
            {
                /* first time seen IVs */

                if( f_ivs_out != NULL )
                {
                    unsigned char iv_info[64];

                    if( memcmp( prev_bssid, ap_cur->bssid, 6 ) == 0 )
                    {
                        iv_info[0] = 0xFF;
                        memcpy( iv_info + 1, &h80211[z    ], 3 );
                        memcpy( iv_info + 4, &h80211[z + 4], 2 );
                        n =  6;
                    }
                    else
                    {
                        memcpy( prev_bssid , ap_cur->bssid,  6 );
                        memcpy( iv_info    , ap_cur->bssid,  6 );
                        memcpy( iv_info + 6, &h80211[z    ], 3 );
                        memcpy( iv_info + 9, &h80211[z + 4], 2 );
                        n = 11;
                    }

                    if( fwrite( iv_info, 1, n, f_ivs_out ) != (size_t) n )
                    {
                        perror( "fwrite(IV info) failed" );
                        return( 1 );
                    }
                }

                uniqueiv_mark( ap_cur->uiv_root, &h80211[z] );

                ap_cur->nb_data++;
            }
        }
        else
            ap_cur->nb_data++;
    }

write_packet:

    if( f_cap_out != NULL )
    {
        struct timeval tv;

        gettimeofday( &tv, NULL );

        if( tv_sec == 0 )
        {
            pkh.tv_sec  = tv.tv_sec;
            pkh.tv_usec = ( tv.tv_usec & ~0x1ff ) + power;
        }
        else
        {
            pkh.tv_sec  = tv_sec;
            pkh.tv_usec = tv_usec;
        }

        if( pfh_out.magic == TCPDUMP_CIGAM )
        {
            SWAP32( pkh.tv_sec  );
            SWAP32( pkh.tv_usec );
            SWAP32( pkh.caplen  );
            SWAP32( pkh.len     );
        }

        n = sizeof( pkh );

        if( fwrite( &pkh, 1, n, f_cap_out ) != (size_t) n )
        {
            perror( "fwrite(packet header) failed" );
            return( 1 );
        }

        fflush( stdout );

        n = pkh.caplen;

        if( fwrite( h80211, 1, n, f_cap_out ) != (size_t) n )
        {
            perror( "fwrite(packet data) failed" );
            return( 1 );
        }

        fflush( stdout );
    }

    return( 0 );
}

void dump_print( int ws_row, int ws_col )
{
    int nlines;
    char strbuf[512];

    /* print some informations about each detected AP */

    fprintf( stderr, "\n BSSID              PWR  Beacons"
                     "   # Data  CH  MB  ENC   ESSID\n\n" );

    nlines = 5;

    ap_cur = ap_end;

    while( ap_cur != NULL )
    {
        if( f_cap_in == NULL && ( ap_cur->nb_pkt < 2 ||
              time( NULL ) - ap_cur->tlast > 120 ) )
        {
            ap_cur = ap_cur->prev;
            continue;
        }

        if( ws_row != 0 && nlines > ws_row )
            return;

        nlines++;

        fprintf( stderr, " %02X:%02X:%02X:%02X:%02X:%02X",
                ap_cur->bssid[0], ap_cur->bssid[1],
                ap_cur->bssid[2], ap_cur->bssid[3],
                ap_cur->bssid[4], ap_cur->bssid[5] );

        fprintf( stderr, "  %3d %8ld %8ld",
                 ap_cur->power,
                 ap_cur->nb_bcn,
                 ap_cur->nb_data );

        fprintf( stderr, " %3d %3d  ", ap_cur->chanl, ap_cur->speed );

        switch( ap_cur->crypt )
        {
            case  0: fprintf( stderr, "OPN " ); break;
            case  1: fprintf( stderr, "WEP?" ); break;
            case  2: fprintf( stderr, "WEP " ); break;
            case  3: fprintf( stderr, "WPA " ); break;
            default: fprintf( stderr, "    " ); break;
        }

        memset( strbuf, 0, sizeof( strbuf ) );
        snprintf( strbuf,  sizeof( strbuf ) - 1,
                  "%-32s", ap_cur->essid );
        strbuf[ws_col - 58] = '\0';
        fprintf( stderr, "  %s\n", strbuf );

        ap_cur = ap_cur->prev;
    }

    /* print some informations about each detected station */

    memset( strbuf, 0, ws_col );

    nlines += 3;

    if( ws_row != 0 && nlines > ws_row ) return;

    memset( strbuf, 0x20, ws_col - 1 );
    fprintf( stderr, "%s\n", strbuf );

    memcpy( strbuf, " BSSID              STATION "
            "           PWR  Packets  ESSID", 58 );
    fprintf( stderr, "%s\n", strbuf );

    memset( strbuf, 0x20, ws_col - 1 );
    fprintf( stderr, "%s\n", strbuf );

    ap_cur = ap_end;

    while( ap_cur != NULL )
    {
        if( f_cap_in == NULL && ( ap_cur->nb_pkt < 2 ||
              time( NULL ) - ap_cur->tlast > 120 ) )
        {
            ap_cur = ap_cur->prev;
            continue;
        }

        if( ws_row != 0 && nlines > ws_row )
            return;

        st_cur = st_end;

        while( st_cur != NULL )
        {
            if( st_cur->base != ap_cur || ( f_cap_in == NULL &&
                  time( NULL ) - ap_cur->tlast > 120 ) )
            {
                st_cur = st_cur->prev;
                continue;
            }

            if( ws_row != 0 && nlines > ws_row )
                return;

            nlines++;

            fprintf( stderr, " %02X:%02X:%02X:%02X:%02X:%02X",
                    ap_cur->bssid[0], ap_cur->bssid[1],
                    ap_cur->bssid[2], ap_cur->bssid[3],
                    ap_cur->bssid[4], ap_cur->bssid[5] );

            fprintf( stderr, "  %02X:%02X:%02X:%02X:%02X:%02X",
                    st_cur->stmac[0], st_cur->stmac[1],
                    st_cur->stmac[2], st_cur->stmac[3],
                    st_cur->stmac[4], st_cur->stmac[5] );

            if( st_cur->power != -1 )
                fprintf( stderr, "  %3d", st_cur->power );
            else
                fprintf( stderr, "     " );

            fprintf( stderr, " %8ld", st_cur->nb_pkt );

            memset( strbuf, 0, sizeof( strbuf ) );
            snprintf( strbuf,  sizeof( strbuf ) - 1,
                      "%-32s", ap_cur->essid );
            strbuf[ws_col - 54] = '\0';
            fprintf( stderr, "  %s\n", strbuf );

            st_cur = st_cur->prev;
        }

        ap_cur = ap_cur->prev;
    }
}

void dump_write_csv( void )
{
    struct tm *ltime;

    if( f_csv_out == NULL )
        return;

    fseek( f_csv_out, 0, SEEK_SET );

    fprintf( f_csv_out,
        "\r\nBSSID, First time seen, Last time seen, Channel, Speed, "
        "Privacy, Power, # beacons, # data, LAN IP, ESSID\r\n" );

    ap_cur = ap_1st;

    while( ap_cur != NULL )
    {
        if( ap_cur->nb_pkt < 2 )
        {
            ap_cur = ap_cur->next;
            continue;
        }

        fprintf( f_csv_out, "%02X:%02X:%02X:%02X:%02X:%02X, ",
                 ap_cur->bssid[0], ap_cur->bssid[1],
                 ap_cur->bssid[2], ap_cur->bssid[3],
                 ap_cur->bssid[4], ap_cur->bssid[5] );

        ltime = localtime( &ap_cur->tinit );

        fprintf( f_csv_out, "%04d-%02d-%02d %02d:%02d:%02d, ",
                 1900 + ltime->tm_year, 1 + ltime->tm_mon,
                 ltime->tm_mday, ltime->tm_hour,
                 ltime->tm_min,  ltime->tm_sec );

        ltime = localtime( &ap_cur->tlast );

        fprintf( f_csv_out, "%04d-%02d-%02d %02d:%02d:%02d, ",
                 1900 + ltime->tm_year, 1 + ltime->tm_mon,
                 ltime->tm_mday, ltime->tm_hour,
                 ltime->tm_min,  ltime->tm_sec );

        fprintf( f_csv_out, "%2d, %3d, ",
                 ap_cur->chanl,
                 ap_cur->speed );

        switch( ap_cur->crypt )
        {
            case  0: fprintf( f_csv_out, "OPN " ); break;
            case  1: fprintf( f_csv_out, "WEP?" ); break;
            case  2: fprintf( f_csv_out, "WEP " ); break;
            case  3: fprintf( f_csv_out, "WPA " ); break;
            default: fprintf( f_csv_out, "    " ); break;
        }

        fprintf( f_csv_out, ", %3d, %8ld, %8ld, ",
                 ap_cur->power,
                 ap_cur->nb_bcn,
                 ap_cur->nb_data );

        fprintf( f_csv_out, "%3d.%3d.%3d.%3d, ",
                 ap_cur->lanip[0], ap_cur->lanip[1],
                 ap_cur->lanip[2], ap_cur->lanip[2] );

        fprintf( f_csv_out, "%-32s\r\n", ap_cur->essid );

        ap_cur = ap_cur->next;
    }

    fprintf( f_csv_out,
        "\r\nStation MAC, First time seen, Last time seen, "
        "Power, # packets, BSSID, ESSID\r\n" );

    st_cur = st_1st;

    while( st_cur != NULL )
    {
        ap_cur = st_cur->base;

        if( ap_cur->nb_pkt < 2 )
        {
            st_cur = st_cur->next;
            continue;
        }

        fprintf( f_csv_out, "%02X:%02X:%02X:%02X:%02X:%02X, ",
                 st_cur->stmac[0], st_cur->stmac[1],
                 st_cur->stmac[2], st_cur->stmac[3],
                 st_cur->stmac[4], st_cur->stmac[5] );

        ltime = localtime( &st_cur->tinit );

        fprintf( f_csv_out, "%04d-%02d-%02d %02d:%02d:%02d, ",
                 1900 + ltime->tm_year, 1 + ltime->tm_mon,
                 ltime->tm_mday, ltime->tm_hour,
                 ltime->tm_min,  ltime->tm_sec );

        ltime = localtime( &st_cur->tlast );

        fprintf( f_csv_out, "%04d-%02d-%02d %02d:%02d:%02d, ",
                 1900 + ltime->tm_year, 1 + ltime->tm_mon,
                 ltime->tm_mday, ltime->tm_hour,
                 ltime->tm_min,  ltime->tm_sec );

        fprintf( f_csv_out, "%3d, %8ld, ",
                 st_cur->power,
                 st_cur->nb_pkt );

        fprintf( f_csv_out, "%02X:%02X:%02X:%02X:%02X:%02X, ",
                 ap_cur->bssid[0], ap_cur->bssid[1],
                 ap_cur->bssid[2], ap_cur->bssid[3],
                 ap_cur->bssid[4], ap_cur->bssid[5] );

        fprintf( f_csv_out, "%-32s\r\n", ap_cur->essid );

        st_cur = st_cur->next;
    }

    fprintf( f_csv_out, "\r\n" );

    fflush( f_csv_out );
}

struct arguments
{
    int card_index;
    int card_model;
    int channels[29];
    char *oprefix;
    int ivs_only;
}
arg;

int rawlen;
unsigned char rawbuf[65536];
unsigned char buffer[65536];

HANDLE semPacket1;
HANDLE semPacket2;

int prompt_exit( int retval )
{
    int i;
    printf( "\n  Press Ctrl-C to exit.\n" );
    scanf( "%d", &i );
    exit( retval );
}

int parse_channels( char *s )
{
    int i, j, n, k, exist;

	for (i = 0; i < 29; i++)
		arg.channels[i] = 0;


	i = 0;

    while( sscanf( s, "%d", &n ) == 1 )
    {
        if( n == 0 )
        {
			// Hop in 2.4Ghz channels
            for( j = 0; j < 14; j++ )
                arg.channels[j] = bg_chans[j];

            return( 0 );
        }

		if (n == -1)
		{
			// Hop in 5Ghz channels
			for (j = 0; j < 28; j++)
				arg.channels[j] = a_chans[j];

			return 0;
		}

		// Search on abg chans if the channel exist
		k = exist = 0;
		while (abg_chans[k] != 0)
		{
			if (n == abg_chans[k])
			{
				exist = 1;
				break;
			}
			++k;
		}

		// Invalid channel;
		if (!exist)
			return 1;
		

        arg.channels[i] = n;
        arg.channels[++i] = 0;

        if( i == 28 ) break;

        while( isdigit( *s ) != 0 )
            s++;

        while( isdigit( *s ) == 0 )
        {
            if( *s == '\0' )
                return( 0 );

            s++;
        }
    }

    return( i == 0 );
}

int WINAPI capture_thread( unsigned char *data, int len, int caplen,
                           __int64 timestamp, int flags, int arg7 )
{
    if( ( flags & 1 ) == 0 )
    {
        rawlen = len;
        memcpy( rawbuf, data, rawlen );

        ReleaseSemaphore( semPacket1, 1, NULL );
        WaitForSingleObject( semPacket2, INFINITE );
    }

    return( 1 );
}

int do_exit = 0;

int WINAPI sighandler( int signum )
{
    stop_monitor();
    do_exit = 1;
    return( TRUE );
}

int main( int argc, char *argv[] )
{
	const char usage[] = "\nusage: airodump-ng -c <channel(s)> -w <output> [--ivs] <interface>\n";
	char *output_filename = NULL;
	char *channel_list = NULL;
	unsigned char *h80211;
    unsigned long tick_prev;
    int caplen, chan_index;
	unsigned short int i;
    int ws_row, ws_col;
    time_t tt;
	char power;

	// Modified to only use Airpcap adapters

    if( argc > 1 && argc <= 7 )
    {
		char *card_interface;

		arg.card_model = 'A';
		arg.card_index = 0;
		arg.oprefix = NULL;

		// Grab the connected Airpcap adapter

		card_interface = ( (char *)argv[argc - 1] + 11 );

		if( strlen(card_interface) >= 2 )
		{
			arg.card_index = ( atoi(card_interface) >= 0 ) ? ( atoi(card_interface) + 1 ) : 0;
		}

		else 
		{
			fprintf( stderr, usage );
			return ( 1 );
		}

		if( arg.card_index > 0 )
		{
			open_adapter( arg.card_index );
		}
		
		else 
		{
			fprintf( stderr, usage );
			return ( 1 );
		}

		// Airpcap adapter found!
		
		for( i = 0; i < argc; i++ ) 
		{
			// Get channel(s) to listen on

			if( strcmp( argv[i], "-c" ) == 0 )
			{
				if( (i + 1) <= argc )
				{
					if( parse_channels( argv[i + 1] ) != 0 )
					{
						fprintf( stderr, usage );
						return ( 1 );
					}

					channel_list = argv[i + 1];
					continue;
				}
			}

			// Get output filename

			else if( strcmp( argv[i], "-w" ) == 0 )
			{
				if( (i + 1) <= argc )
				{
					arg.oprefix = argv[i + 1];
					output_filename = arg.oprefix;
					continue;
				}

				fprintf( stderr, usage );
				return ( 1 );
			}

			// Add the --ivs for cracking WEP

			else if( strcmp( argv[i], "--ivs" ) == 0 )
			{	
				arg.ivs_only = ( strcmp(argv[i], "--ivs") == 0 ) ? 1 : 0;
				continue;
			}
		}
    }

	else
	{
		fprintf( stderr, usage );
		return ( 1 );
	}

	if( channel_list == NULL )
	{
		channel_list = "0";

		if( parse_channels( channel_list ) != 0 )
		{
			fprintf( stderr, usage );
			return ( 1 );
		}
	}

	if( output_filename == NULL ) 
	{
		output_filename = ".tmp";
		arg.oprefix = output_filename;
	}

    if( dump_initialize( arg.oprefix, arg.ivs_only ) ) return ( 1 );

    semPacket1 = CreateSemaphore( NULL, 0, 1, NULL );
    semPacket2 = CreateSemaphore( NULL, 0, 1, NULL );

    SetConsoleCtrlHandler( (PHANDLER_ROUTINE) sighandler, TRUE );

    if( start_monitor( capture_thread ) != 0 ) return ( 1 );

    tick_prev = GetTickCount();
    set_channel( arg.channels[chan_index = 0] );

    set_cursor_pos( 0, 0 );
    clear_console( NULL, NULL );

    tt = time( NULL );

    while( !do_exit )
    {
        if( time( NULL ) - tt >= 20 )
        {
            tt = time( NULL );
            dump_write_csv();
        }

        if( GetTickCount() - tick_prev >= 300 ||
            GetTickCount() < tick_prev )
        {
            tick_prev = GetTickCount();

            if( arg.channels[++chan_index] == 0 )
                chan_index = 0;

            set_channel( arg.channels[chan_index] );

            sprintf( buffer, " Channel : %02d - airodump-ng %s ",
                       arg.channels[chan_index],
					   VERSION);

            SetConsoleTitle( buffer );
            clear_console( &ws_row, &ws_col );
            set_cursor_pos( 0, 0 );

            dump_print( ws_row, ws_col );
        }

		if( GetNextPacket(&h80211, &caplen, &power) != 0) continue;

        if( dump_add_packet( h80211, caplen, power,
                             arg.channels[chan_index], 0, 0 ) != 0 )
        {
            dump_write_csv();

            if( f_csv_out != NULL ) fclose( f_csv_out );
            if( f_cap_out != NULL ) fclose( f_cap_out );
            if( f_ivs_out != NULL ) fclose( f_ivs_out );

            stop_monitor();
            return ( 1 );
        }
    }

    dump_write_csv();

    if( f_csv_out != NULL ) fclose( f_csv_out );
    if( f_cap_out != NULL ) fclose( f_cap_out );
    if( f_ivs_out != NULL ) fclose( f_ivs_out );

    stop_monitor();
    return( 0 );
}
