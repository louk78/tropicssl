/*
 *  AES-256 file encryption program
 *
 *  Based on XySSL: Copyright (C) 2006-2008  Christophe Devine
 *
 *  Copyright (C) 2009  Paul Bakker <polarssl_maintainer at polarssl dot org>
 *
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *  
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *    * Neither the names of PolarSSL or XySSL nor the names of its contributors
 *      may be used to endorse or promote products derived from this software
 *      without specific prior written permission.
 *  
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 *  TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 *  PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 *  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 *  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 *  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef _CRT_SECURE_NO_DEPRECATE
#define _CRT_SECURE_NO_DEPRECATE 1
#endif

#if defined(WIN32)
#include <windows.h>
#include <io.h>
#else
#include <sys/types.h>
#include <unistd.h>
#endif

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#include "tropicssl/aes.h"
#include "tropicssl/sha2.h"

#define MODE_ENCRYPT    0
#define MODE_DECRYPT    1

#define USAGE   \
    "\n  aescrypt2 <mode> <input filename> <output filename> <key>\n" \
    "\n   <mode>: 0 = encrypt, 1 = decrypt\n" \
    "\n  example: aescrypt2 0 file file.aes hex:E76B2413958B00E193\n" \
    "\n"

int main( int argc, char *argv[] )
{
    int ret = 1, i, n;
    int keylen, mode, lastn;
    FILE *fkey, *fin, *fout;

    char *p;
    unsigned char IV[16];
    unsigned char key[512];
    unsigned char digest[32];
    unsigned char buffer[1024];

    aes_context aes_ctx;
    sha2_context sha_ctx;

#if defined(WIN32)
       LARGE_INTEGER li_size;
    __int64 filesize, offset;
#else
      off_t filesize, offset;
#endif

    /*
     * Parse the command-line arguments.
     */
    if( argc != 5 )
    {
        printf( USAGE );

#if defined(WIN32)
        printf( "\n  Press Enter to exit this program.\n" );
        fflush( stdout ); getchar();
#endif

        goto exit;
    }

    mode = atoi( argv[1] );

    if( mode != MODE_ENCRYPT && mode != MODE_DECRYPT )
    {
        fprintf( stderr, "invalide operation mode\n" );
        goto exit;
    }

    if( strcmp( argv[2], argv[3] ) == 0 )
    {
        fprintf( stderr, "input and output filenames must differ\n" );
        goto exit;
    }

    if( ( fin = fopen( argv[2], "rb" ) ) == NULL )
    {
        fprintf( stderr, "fopen(%s,rb) failed\n", argv[2] );
        goto exit;
    }

    if( ( fout = fopen( argv[3], "wb+" ) ) == NULL )
    {
        fprintf( stderr, "fopen(%s,wb+) failed\n", argv[3] );
        goto exit;
    }

    /*
     * Read the secret key and clean the command line.
     */
    if( ( fkey = fopen( argv[4], "rb" ) ) != NULL )
    {
        keylen = fread( key, 1, sizeof( key ), fkey );
        fclose( fkey );
    }
    else
    {
        if( memcmp( argv[4], "hex:", 4 ) == 0 )
        {
            p = &argv[4][4];
            keylen = 0;

            while( sscanf( p, "%02X", &n ) > 0 &&
                   keylen < (int) sizeof( key ) )
            {
                key[keylen++] = (unsigned char) n;
                p += 2;
            }
        }
        else
        {
            keylen = strlen( argv[4] );

            if( keylen > (int) sizeof( key ) )
                keylen = (int) sizeof( key );

            memcpy( key, argv[4], keylen );
        }
    }

    memset( argv[4], 0, strlen( argv[4] ) );

#if defined(WIN32)
    /*
     * Support large files (> 2Gb) on Win32
     */
    li_size.QuadPart = 0;
    li_size.LowPart  =
        SetFilePointer( (HANDLE) _get_osfhandle( _fileno( fin ) ),
                        li_size.LowPart, &li_size.HighPart, FILE_END );

    if( li_size.LowPart == 0xFFFFFFFF && GetLastError() != NO_ERROR )
    {
        fprintf( stderr, "SetFilePointer(0,FILE_END) failed\n" );
        goto exit;
    }

    filesize = li_size.QuadPart;
#else
    if( ( filesize = lseek( fileno( fin ), 0, SEEK_END ) ) < 0 )
    {
        perror( "lseek" );
        goto exit;
    }
#endif

    if( fseek( fin, 0, SEEK_SET ) < 0 )
    {
        fprintf( stderr, "fseek(0,SEEK_SET) failed\n" );
        goto exit;
    }

    if( mode == MODE_ENCRYPT )
    {
        /*
         * Generate the initialization vector as:
         * IV = SHA-256( filesize || filename )[0..15]
         */
        for( i = 0; i < 8; i++ )
            buffer[i] = (unsigned char)( filesize >> ( i << 3 ) );

        p = argv[2];

        sha2_starts( &sha_ctx, 0 );
        sha2_update( &sha_ctx, buffer, 8 );
        sha2_update( &sha_ctx, (unsigned char *) p, strlen( p ) );
        sha2_finish( &sha_ctx, digest );

        memcpy( IV, digest, 16 );

        /*
         * The last four bits in the IV are actually used
         * to store the file size modulo the AES block size.
         */
        lastn = (int)( filesize & 0x0F );

        IV[15] = (unsigned char)
            ( ( IV[15] & 0xF0 ) | lastn );

        /*
         * Append the IV at the beginning of the output.
         */
        if( fwrite( IV, 1, 16, fout ) != 16 )
        {
            fprintf( stderr, "fwrite(%d bytes) failed\n", 16 );
            goto exit;
        }

        /*
         * Hash the IV and the secret key together 8192 times
         * using the result to setup the AES context and HMAC.
         */
        memset( digest, 0,  32 );
        memcpy( digest, IV, 16 );

        for( i = 0; i < 8192; i++ )
        {
            sha2_starts( &sha_ctx, 0 );
            sha2_update( &sha_ctx, digest, 32 );
            sha2_update( &sha_ctx, key, keylen );
            sha2_finish( &sha_ctx, digest );
        }

        memset( key, 0, sizeof( key ) );
          aes_setkey_enc( &aes_ctx, digest, 256 );
        sha2_hmac_starts( &sha_ctx, digest, 32, 0 );

        /*
         * Encrypt and write the ciphertext.
         */
        for( offset = 0; offset < filesize; offset += 16 )
        {
            n = ( filesize - offset > 16 ) ? 16 : (int)
                ( filesize - offset );

            if( fread( buffer, 1, n, fin ) != (size_t) n )
            {
                fprintf( stderr, "fread(%d bytes) failed\n", n );
                goto exit;
            }

            for( i = 0; i < 16; i++ )
                buffer[i] = (unsigned char)( buffer[i] ^ IV[i] );

            aes_crypt_ecb( &aes_ctx, AES_ENCRYPT, buffer, buffer );
            sha2_hmac_update( &sha_ctx, buffer, 16 );

            if( fwrite( buffer, 1, 16, fout ) != 16 )
            {
                fprintf( stderr, "fwrite(%d bytes) failed\n", 16 );
                goto exit;
            }

            memcpy( IV, buffer, 16 );
        }

        /*
         * Finally write the HMAC.
         */
        sha2_hmac_finish( &sha_ctx, digest );

        if( fwrite( digest, 1, 32, fout ) != 32 )
        {
            fprintf( stderr, "fwrite(%d bytes) failed\n", 16 );
            goto exit;
        }
    }

    if( mode == MODE_DECRYPT )
    {
        unsigned char tmp[16];

        /*
         *  The encrypted file must be structured as follows:
         *
         *        00 .. 15              Initialization Vector
         *        16 .. 31              AES Encrypted Block #1
         *           ..
         *      N*16 .. (N+1)*16 - 1    AES Encrypted Block #N
         *  (N+1)*16 .. (N+1)*16 + 32   HMAC-SHA-256(ciphertext)
         */
        if( filesize < 48 )
        {
            fprintf( stderr, "File too short to be encrypted.\n" );
            goto exit;
        }

        if( ( filesize & 0x0F ) != 0 )
        {
            fprintf( stderr, "File size not a multiple of 16.\n" );
            goto exit;
        }

        /*
         * Substract the IV + HMAC length.
         */
        filesize -= ( 16 + 32 );

        /*
         * Read the IV and original filesize modulo 16.
         */
        if( fread( buffer, 1, 16, fin ) != 16 )
        {
            fprintf( stderr, "fread(%d bytes) failed\n", 16 );
            goto exit;
        }

        memcpy( IV, buffer, 16 );
        lastn = IV[15] & 0x0F;

        /*
         * Hash the IV and the secret key together 8192 times
         * using the result to setup the AES context and HMAC.
         */
        memset( digest, 0,  32 );
        memcpy( digest, IV, 16 );

        for( i = 0; i < 8192; i++ )
        {
            sha2_starts( &sha_ctx, 0 );
            sha2_update( &sha_ctx, digest, 32 );
            sha2_update( &sha_ctx, key, keylen );
            sha2_finish( &sha_ctx, digest );
        }

        memset( key, 0, sizeof( key ) );
          aes_setkey_dec( &aes_ctx, digest, 256 );
        sha2_hmac_starts( &sha_ctx, digest, 32, 0 );

        /*
         * Decrypt and write the plaintext.
         */
        for( offset = 0; offset < filesize; offset += 16 )
        {
            if( fread( buffer, 1, 16, fin ) != 16 )
            {
                fprintf( stderr, "fread(%d bytes) failed\n", 16 );
                goto exit;
            }

            memcpy( tmp, buffer, 16 );
 
            sha2_hmac_update( &sha_ctx, buffer, 16 );
            aes_crypt_ecb( &aes_ctx, AES_DECRYPT, buffer, buffer );
   
            for( i = 0; i < 16; i++ )
                buffer[i] = (unsigned char)( buffer[i] ^ IV[i] );

            memcpy( IV, tmp, 16 );

            n = ( lastn > 0 && offset == filesize - 16 )
                ? lastn : 16;

            if( fwrite( buffer, 1, n, fout ) != (size_t) n )
            {
                fprintf( stderr, "fwrite(%d bytes) failed\n", n );
                goto exit;
            }
        }

        /*
         * Verify the message authentication code.
         */
        sha2_hmac_finish( &sha_ctx, digest );

        if( fread( buffer, 1, 32, fin ) != 32 )
        {
            fprintf( stderr, "fread(%d bytes) failed\n", 32 );
            goto exit;
        }

        if( memcmp( digest, buffer, 32 ) != 0 )
        {
            fprintf( stderr, "HMAC check failed: wrong key, "
                             "or file corrupted.\n" );
            goto exit;
        }
    }

    ret = 0;

exit:

    memset( buffer, 0, sizeof( buffer ) );
    memset( digest, 0, sizeof( digest ) );

    memset( &aes_ctx, 0, sizeof(  aes_context ) );
    memset( &sha_ctx, 0, sizeof( sha2_context ) );

    return( ret );
}
