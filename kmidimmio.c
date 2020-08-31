/****************************************************************************
**
** K MIDI MMIO - Simple midi player MMIO version
**
** Copyright (C) 2020 by KO Myung-Hun <komh@chollian.net>
**
** This file is part of K MIDI DECoder.
**
** $BEGIN_LICENSE$
**
** GNU General Public License Usage
** This file may be used under the terms of the GNU
** General Public License version 3.0 as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU General Public License version 3.0 requirements will be
** met: http://www.gnu.org/copyleft/gpl.html.
**
** $END_LICENSE$
**
****************************************************************************/

/** @file kmidimmio.c */

#include <os2.h>

#define INCL_OS2MM
#include <os2me.h>

#include <stdio.h>
#include <stdlib.h>

#include <kai.h>

#include "kmididec.h"

#define SAMPLE_RATE 44100
#define SAMPLES 2048
#define SAMPLE_SIZE ( 2/* 16bits */ * 2/* 2 CH */ )

#define USE_FLOAT 1

/* callback for KAI */
static ULONG APIENTRY kaiCallback( PVOID pCBData,
                                   PVOID pBuffer, ULONG ulBufferSize )
{
#if USE_FLOAT
    float buf[ ulBufferSize / sizeof( short )];

    return kaiFloatToS16( pBuffer, ulBufferSize,
                          buf, kmdecDecode( pCBData, buf, sizeof( buf )));
#else
    return kmdecDecode( pCBData, pBuffer, ulBufferSize );
#endif
}

/* convert ms to time */
static void msToTime( int ms, int *h, int *m, int *s, int *hund )
{
    int sec = ms / 1000;
    int hd = ( ms % 1000 ) / 10;
    int min = sec / 60;
    sec %= 60;
    int hour = min / 60;
    min %= 60;

    if( h )
        *h = hour;

    if( m )
        *m = min;

    if( s )
        *s = sec;

    if( hund )
        *hund = hd;
}

#include <string.h>
#include <errno.h>

static int ioOpen( const char *name )
{
    MMIOINFO info;
    HMMIO hmmio;

    memset( &info, 0, sizeof( info ));
    info.ulTranslate = MMIO_TRANSLATEHEADER;

    hmmio = mmioOpen(( PSZ )name, &info, 0 );

    return hmmio == ( HMMIO )NULL ? -1 : hmmio;
}

static int ioRead( int fd, void *buf, size_t n )
{
    ULONG rc = mmioRead( fd, buf, n );

    return rc == MMIO_ERROR ? -1 : rc;
}

static int ioSeek( int fd, long offset, int origin )
{
    static LONG lOrigins[] = { SEEK_SET, SEEK_CUR, SEEK_END };

    if( origin >= sizeof( lOrigins ) / sizeof( lOrigins[ 0 ]))
    {
        errno = EINVAL;
        return -1;
    }

    ULONG rc = mmioSeek( fd, offset, lOrigins[ origin ]);

    return rc == MMIO_ERROR ? -1 : rc;
}

static int ioTell( int fd )
{
    ULONG rc = mmioSeek( fd, 0, SEEK_CUR );

    return rc == MMIO_ERROR ? -1 : rc;
}

static int ioClose( int fd )
{
    return mmioClose( fd, 0 ) ? -1 : 0;
}

static KMDECIOFUNCS io = {
    .open = ioOpen,
    .read = ioRead,
    .seek = ioSeek,
    .tell = ioTell,
    .close = ioClose,
};

int main( int argc, char *argv[])
{
    PKMDEC dec;
    KMDECAUDIOINFO audioInfo =
    {
#if USE_FLOAT
        .bps = KMDEC_BPS_FLOAT,
#else
        .bps = KMDEC_BPS_S16,
#endif
        .channels = 2,
        .sampleRate = SAMPLE_RATE
    };

    KAISPEC  ksWanted, ksObtained;
    HKAI     hkai;

    int rc = 1;

    if( argc < 3 )
    {
        fprintf( stderr, "Usage : kmidi MIDI-file sound-font-file\n");

        return rc;
    }

    dec = kmdecOpenEx( argv[ 1 ], argv[ 2 ], &audioInfo, &io );
    if( !dec )
    {
        fprintf( stderr, "Failed to init kmdec\n");

        return rc;
    }

    if( kaiInit( KAIM_AUTO ))
    {
        fprintf( stderr, "Failed to init kai\n");

        goto exit_kmdec_close;
    }

    ksWanted.usDeviceIndex      = 0;
    ksWanted.ulType             = KAIT_PLAY;
    ksWanted.ulBitsPerSample    = BPS_16;
    ksWanted.ulSamplingRate     = audioInfo.sampleRate;
    ksWanted.ulDataFormat       = 0;
    ksWanted.ulChannels         = audioInfo.channels;
    ksWanted.ulNumBuffers       = 2;
    ksWanted.ulBufferSize       = SAMPLES * SAMPLE_SIZE;
    ksWanted.fShareable         = TRUE;
    ksWanted.pfnCallBack        = kaiCallback;
    ksWanted.pCallBackData      = dec;

    if( kaiOpen( &ksWanted, &ksObtained, &hkai ))
    {
        fprintf( stderr, "Failed to open audio device!!!\n");

        goto exit_kai_done;
    }

    int H, M, S, HUND;
    int h, m, s, hund;

    msToTime( kmdecGetDuration( dec ), &H, &M, &S, &HUND );

    kaiPlay( hkai );

    printf("ESC = quit, q = stop, w = play, e = pause, r = resume, "
           "a = -5s, s = +5s\n");

    while( kaiStatus( hkai ) != KAIS_COMPLETED )
    {
        msToTime( kmdecGetPosition( dec ), &h, &m, &s, &hund );
        printf("Playing time: %02d:%02d:%02d.%02d of %02d:%02d:%02d.%02d\r",
               h, m, s, hund, H, M, S, HUND );

        int key = _read_kbd( 0, 0, 1 );
        if( key == 0 )
             key = _read_kbd( 0, 1, 1 );

        if( key == 27 )    /* ESC */
            break;

        switch( key )
        {
            case 'q':
                kaiStop( hkai );
                break;

            case 'w':
                kaiPlay( hkai );
                break;

            case 'e':
                kaiPause( hkai );
                break;

            case 'r':
                kaiResume( hkai );
                break;

            case 'a':
            case 's':
                kaiStop( hkai );
                kmdecSeek( dec, key == 'a' ? -5000 : 5000, KMDEC_SEEK_CUR );
                kaiPlay( hkai );

                /* consume already pressed keys */
                while( _read_kbd( 0, 0, 1 ) != -1 )
                    /* nothing */;
                break;
        }

        _sleep2( 1 );
    }
    printf("\n");

    kaiClose( hkai );

    rc = 0;

exit_kai_done :
    kaiDone();

exit_kmdec_close:
    kmdecClose( dec );

    return rc;
}
