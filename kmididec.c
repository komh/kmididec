/****************************************************************************
**
** K MIDI DECoder - MIDI to PCM decoder using fluidsynth
**
** Copyright (C) 2018 by KO Myung-Hun <komh@chollian.net>
**
** This file is part of K MIDI DECoder.
**
** $BEGIN_LICENSE$
**
** GNU Lesser General Public License Usage
** This file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** $END_LICENSE$
**
****************************************************************************/

/** @file kmididec.c */

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <sys/param.h>
#include <io.h>
#include <fcntl.h>
#include <errno.h>

#include <fluidsynth.h>
/* missed API declaration in 1.0.9 */
FLUIDSYNTH_API
int fluid_synth_channel_pressure( fluid_synth_t *synth, int chan, int val );

#include "kmididec.h"

/* OS/2 real-time midi format */
#define OS2MIDI 0xFFFF

/**
 * Header information
 */
typedef struct kmthd
{
    uint16_t format;    /**< format */
    uint16_t tracks;    /**< a number of tracks */
    uint16_t division;  /**< time format */
} KMTHD, *PKMTHD;

/* marker of end of track for next tick */
#define END_OF_TRACK (( uint32_t )-1 )

/**
 * Track information
 */
typedef struct kmtrk
{
    struct kmdec *dec;  /**< MIDI decoder */
    uint64_t start;     /**< start position of a track */
    uint32_t length;    /**< length of a track */
    uint32_t offset;    /**< offset relative to start position */
    uint32_t nextTick;  /**< next tick */
    uint8_t status;     /**< status byte */
} KMTRK, *PKMTRK;

/**
 * Memory FD
 */
typedef struct kmemfd
{
    uint8_t *buffer;    /**< buffer for a file */
    uint32_t size;      /**< size of a buffer @a buffer in bytes */
    uint32_t length;    /**< bytes filled in a buffer @a buffer */
    uint32_t offset;    /**< current position */
} KMEMFD, *PKMEMFD;

/* defaul values */
#define DEFAULT_TEMPO       500000 /* us/qn */
#define DEFAULT_NUMERATOR   4
#define DEFAULT_DENOMINATOR 4

/* clocks per sec */
#define CLOCK_BASE  INT64_C( 1000000 ) /* us */

/**
 * MIDI decoder
 */
typedef struct kmdec
{
    int fd;             /**< fd for MIDI file */
    bool closeFd;       /**< flag to indicate to close fd */
    PKMDECIOFUNCS io;   /**< IO functions */
    PKMEMFD mfd;        /**< fd for memory IO */

    KMTHD header;   /**< header */
    PKMTRK tracks;  /**< array of a track */

    fluid_settings_t *settings; /**< setting of fluidsynth */
    fluid_synth_t *synth;       /**< synthesizer of fluidsynth */
    int sf;                     /**< sound font file */

    /** synthesize in float or in s16 */
    FLUIDSYNTH_API int ( *synth_write )( fluid_synth_t *, int,
                                         void *, int, int, void *, int, int );

    int clockUnit;  /**< us/MIDI clock */

    int sampleRate; /**< sample rate */
    int sampleSize; /**< bytes per sample */

    uint32_t tempo;         /**< tempo in us/qn */
    uint8_t numerator;      /**< numerator */
    uint8_t denominator;    /**< denominator */

    uint32_t tick;      /**< current tick */
    uint64_t clock;     /**< current clock in us */
    uint64_t duration;  /**< duration of MIDI file in us */

    char *buffer;   /**< buffer for samples */
    int bufLen;     /**< legnth of buffer */
    int bufPos;     /**< position in buffer */
} KMDEC, *PKMDEC;

/* a mode for decode() */
#define DECODE_SEEK    0    /* seek mode */
#define DECODE_PLAY    1    /* play mode */

static PKMEMFD memOpen( int fd, PKMDECIOFUNCS io );
static int memClose( PKMEMFD mfd );
static int memRead( PKMEMFD mfd, void *buf, size_t n );
static int memSeek( PKMEMFD mfd, long offset, int origin );
static int memTell( PKMEMFD mfd );

static int initMidiInfo( PKMDEC dec );
static int reset( PKMDEC dec );

static int readVarQ( PKMTRK track, int *val );
static int decodeDelta( PKMTRK track);
static int decodeMetaEvent( PKMTRK track);
static int decodeEvent( PKMTRK track);
static int decodeOS2SysExEvent( PKMTRK track );
static int decodeOS2Event( PKMTRK track );
static int decode( PKMDEC dec, int mode );

#define MEMFD_BUF_DELTA ( 64 * 1024 )

static PKMEMFD memOpen( int fd, PKMDECIOFUNCS io )
{
    PKMEMFD mfd;
    uint8_t *buffer;

    mfd = calloc( 1, sizeof( *mfd ));
    if( !mfd )
        return NULL;

    while( 1 )
    {
        if( mfd->length == mfd->size )
        {
            mfd->size += MEMFD_BUF_DELTA;

            buffer = realloc( mfd->buffer, mfd->size );
            if( !buffer )
            {
fail:
                free( mfd->buffer );
                free( mfd );

                return NULL;
            }

            mfd->buffer = buffer;
        }

        int size = MEMFD_BUF_DELTA - ( mfd->length % MEMFD_BUF_DELTA );
        int len = io->read( fd, mfd->buffer + mfd->length, size );

        if( len == -1 )
            goto fail;

        if( len == 0 )
            break;

        mfd->length += len;
    }

    /* shrink to fit */
    buffer = realloc( mfd->buffer, mfd->length );
    if( !buffer )
        goto fail;

    mfd->buffer = buffer;

    return mfd;
}

static int memClose( PKMEMFD mfd )
{
    if( !mfd )
        return -1;

    free( mfd->buffer );
    free( mfd );

    return 0;
}

static int memRead( PKMEMFD mfd, void *buf, size_t n )
{
    if( !mfd )
        return -1;

    if( n == 0 || mfd->length == mfd->offset )
        return 0;

    int len = MIN( n, mfd->length - mfd->offset );

    memcpy( buf, mfd->buffer + mfd->offset, len );
    mfd->offset += len;

    return len;
}

static int memSeek( PKMEMFD mfd, long offset, int origin )
{
    long pos;

    if( !mfd )
        return -1;

    switch( origin )
    {
        case SEEK_SET:
            pos = offset;
            break;

        case SEEK_CUR:
            pos = mfd->offset + offset;
            break;

        case SEEK_END:
            pos = mfd->length + offset;
            break;
    }

    if( pos < 0 || pos > mfd->length )
        return -1;

    mfd->offset = pos;

    return pos;
}

static int memTell( PKMEMFD mfd )
{
    if( !mfd )
        return -1;

    return mfd->offset;
}

/**
 * Initialize header and track information
 *
 * @param[in] dec Pointer to decoder
 * @return 0 on success, -1 on error
 */
static int initMidiInfo( PKMDEC dec )
{
    PKMEMFD mfd = dec->mfd;
    PKMTHD header = &dec->header;
    PKMTRK *tracks = &dec->tracks;
    uint8_t data[ 14 ];

    if( memRead( mfd, data, 10 ) == -1 )
        return -1;

    /* Timing Generation Control of OS/2 real-time midi data ? */
    if( memcmp( data, "\xF0\x00\x00\x3A\x03\x01\x18", 7 ) == 0
        && data[ 9 ] == 0xF7 )
    {
        header->format = OS2MIDI;
        header->tracks = 1;

        uint8_t pp = data[ 7 ] & 0x7F;
        if( pp & 0x40 )
            header->division = 24 / ((( pp & 0x3F ) + 1 ) * 3 );
        else
            header->division = 24 * ( pp + 1 );

        /*
         * PPQN below 1 is not supported. If there are real cases, consider
         * then.
         */
        if( header->division == 0 )
        {
            fprintf( stderr, "Not supported time format\n");
            return -1;
        }

        PKMTRK track = calloc( 1, sizeof( *track ));
        if( !track )
            return -1;

        track->dec = dec;
        track->start = memTell( mfd );

        if( memSeek( mfd, 0, SEEK_END ) == -1
            || ( track->length = memTell( mfd ) - track->start,
                memSeek( mfd, track->start, SEEK_SET ) == -1 ))
        {
            free( track );

            return -1;
        }

        *tracks = track;

        return 0;
    }

    if( memRead( mfd, data + 10, 4 ) == -1 )
        return -1;

    if( memcmp( data, "MThd\x00\x00\x00\x06", 8 ) != 0 )
    {
        fprintf( stderr, "Not supported MIDI file\n");

        return -1;
    }

    header->format = ntohs( *( uint16_t * )( data + 8 ));
    header->tracks = ntohs( *( uint16_t * )( data + 10 ));
    header->division = ntohs( *( uint16_t * )( data + 12 ));

    if( header->format >= 2 )
    {
        fprintf( stderr, "Not supported MIDI format\n");

        return -1;
    }

    if(( header->division >> 15 ) & 1)
    {
        fprintf( stderr, "Not supported time format\n");

        return -1;
    }

    *tracks = calloc( header->tracks, sizeof( **tracks ));
    if( !*tracks )
        return -1;

    for( int i = 0; i < header->tracks; i++ )
    {
        PKMTRK track = ( *tracks ) + i;
        if( memRead( mfd, data, 8 ) == -1
            || memcmp( data, "MTrk", 4 ) != 0 )
        {
fail:
            free( *tracks );
            *tracks = NULL;

            return -1;
        }

        track->dec = dec;
        track->start = memTell( mfd );
        track->length = ntohl( *( long * )( data + 4 ));
        if( decodeDelta( track ) == -1 )
            goto fail;

        if( memSeek( mfd, track->start + track->length, SEEK_SET ) == -1 )
            goto fail;
    }

    return 0;
}

/**
 * Reset decoder
 *
 * @param[in] dec Pointer to a decoder
 * @return 0 on success, -1 on error
 */
static int reset( PKMDEC dec )
{
    /* reset tracks */
    for( int i = 0; i < dec->header.tracks; i++ )
    {
        PKMTRK track = dec->tracks + i;

        track->offset = 0;
        track->nextTick = 0;

        if( memSeek( dec->mfd, track->start, SEEK_SET ) == -1 )
            return -1;

        if( dec->header.format != OS2MIDI && decodeDelta( track ) == -1 )
            return -1;

        track->status = 0;
    }

    /* reset fluidsynth */
    fluid_synth_system_reset( dec->synth );

    /* reset to default values */
    dec->tempo = DEFAULT_TEMPO;
    dec->numerator = DEFAULT_NUMERATOR;
    dec->denominator = DEFAULT_DENOMINATOR;

    dec->tick = 0;
    dec->clock = 0;

    dec->bufLen = 0;
    dec->bufPos = 0;

    return 0;
}

/**
 * Read varialbe qunatity
 *
 * @param[in] track Pointer to a track
 * @param[out] val Decoded integer
 * @return 0 on success, -1 on error
 */
static int readVarQ( PKMTRK track, int *val )
{
    PKMEMFD mfd = track->dec->mfd;
    uint8_t b;
    int count = 0;

    int vq = 0;

    do
    {
        if( count >= 4 || memRead( mfd, &b, 1 ) == -1 )
            return -1;
        count++;
        track->offset++;

        vq = ( vq << 7 ) | ( b & 0x7F );
    } while( b & 0x80 );

    *val = vq;

    return 0;
}

/**
 * Decode delta time
 *
 * @param[in] track Pointer to a track
 * @return 0 on success, -1 on error
 */
static int decodeDelta( PKMTRK track )
{
    if( track->offset >= track->length )
    {
        track->nextTick = END_OF_TRACK;

        return 0;
    }

    int delta;
    if( readVarQ( track, &delta ) == -1 )
        return -1;

    /* accumulate delta time */
    track->nextTick += delta;

    return 0;
}

/**
 * Decode meta event
 *
 * @param[in] track Pointer to a track
 * @return 0 on success, -1 on error
 */
static int decodeMetaEvent( PKMTRK track )
{
    PKMEMFD mfd = track->dec->mfd;
    uint8_t type;
    int len;

    if( track->offset >= track->length )
        return 0;

    /* type */
    if( memRead( mfd, &type, 1 ) == -1 )
        return -1;
    track->offset++;

    /* length */
    if( readVarQ( track, &len ) == -1 )
        return -1;

    uint8_t data[ len + 1 ];
    data[ len ] = '\0';

    if( memRead( mfd, data, len ) == -1 )
        return -1;
    track->offset += len;

    switch( type )
    {
        case 0x00: /* sequence number */
            if( len != 2 )
                return -1;
            break;

        case 0x01: /* text event */
        case 0x02: /* copyright notice */
        case 0x03: /* sequence/track name */
        case 0x04: /* instrument name */
        case 0x05: /* lyric */
        case 0x06: /* marker */
        case 0x07: /* cue point */
            /* text */
            break;

        case 0x20: /* MIDI channel prefix */
            if( len != 1 )
                return -1;
            break;

        case 0x2F: /* end of track */
            if( len != 0 || track->offset != track->length )
                return -1;
            break;

        case 0x51: /* set tempo */
        {
            if( len != 3 )
                return -1;

            track->dec->tempo = data[ 0 ] << 16 | data[ 1 ] << 8 | data[ 2 ];
            break;
        }

        case 0x54: /* SMPTE offset */
            if( len != 5 )
                return -1;
            break;

        case 0x58: /* time signature */
        {
            if( len != 4 )
                return -1;

            PKMDEC dec = track->dec;

            dec->numerator = data[ 0 ];
            dec->denominator = 1 << data[ 1 ]; /* power of 2 */
            break;
        }

        case 0x59: /* key signature */
            if( len != 2 )
                return -1;
            break;

        case 0x7F: /* sequencer-specific meta-event */
        /*
         * Some MIDI files such as JSBLUES.MID in \MMOS2\SOUND, does not
         * respect the format of type 0x7F. Disable the length check
         */
#if 0
            if( len >= 5 );
                return -1;
#endif
            break;
    }

    return 0;
}

/**
 * Decode event
 *
 * @param[in] track Pointer to a track
 * @return 0 on success, -1 on error
 */
static int decodeEvent( PKMTRK track )
{
    PKMEMFD mfd = track->dec->mfd;
    fluid_synth_t *synth = track->dec->synth;

    uint8_t status;
    uint8_t event;
    uint8_t channel;
    int len;

    if( track->offset >= track->length )
        return 0;

    if( memSeek( mfd, track->start + track->offset, SEEK_SET ) == -1 )
        return -1;

    if( memRead( mfd, &status, 1 ) == -1 )
        return -1;
    track->offset++;

    /* implicit status ? */
    if( status < 0x80 )
    {
        status = track->status;
        if( memSeek( mfd, -1, KMDEC_SEEK_CUR ) == -1 )
            return -1;
        track->offset--;
    }

    if( status < 0x80 )
        return -1;

    if( status < 0xF0 )
        track->status = status;

    event   = status & 0xF0;
    channel = status & 0x0F;

    /* calculate length of event data */
    if( status == 0xF0 || status == 0xF7 )
    {
        if( readVarQ( track, &len ) == -1 )
            return -1;
    }
    else if( status == 0xFF )
    {
        if( decodeMetaEvent( track ) == -1 )
            return -1;

        len = 0;
    }
    else
    {
        len = 2;
        /*
         * status 0xF2, event 0x80, 0x90, 0xA0, 0xB0, 0xE0: len = 2
         * status 0xF3, event 0xC0, 0xD0: len = 1
         * status 0xF1, 0xF4, 0xF5, 0xF6, 0xF8 - 0xFE: len = 0
         */
        if( status == 0xF3 || event == 0xC0 || event == 0xD0 )
            len = 1;
        else if( status == 0xF1 || ( status >= 0xF4 && status <= 0xF6 )
                 || ( status >= 0xF8 && status <= 0xFE ))
            len = 0;
    }

    uint8_t data[ len ];

    if( memRead( mfd, data, len ) == -1 )
        return -1;
    track->offset += len;

    /* check F0 SysEx syntax which should end with F7 EOX */
    if( status == 0xF0 && data[ len - 1 ] != 0xF7 )
        return -1;

    data[ 0 ] &= 0x7F;
    data[ 1 ] &= 0x7F;

    /* pass MIDI event to fluidsynth */
    switch( event )
    {
        case 0x80:  /* note off */
            fluid_synth_noteoff( synth, channel, data[ 0 ]);
            break;

        case 0x90:  /* note on */
            fluid_synth_noteon( synth, channel, data[ 0 ], data[ 1 ]);
            break;

        case 0xA0: /* polyphonic aftertouch */
            /* not supported */
            break;

        case 0xB0:  /* control mode hnage */
            fluid_synth_cc( synth, channel, data[ 0 ], data[ 1 ]);
            break;

        case 0xC0:  /* program change */
            fluid_synth_program_change( synth, channel, data[ 0 ]);
            break;

        case 0xD0:  /* channel key pressure */
            fluid_synth_channel_pressure( synth, channel, data[ 0 ]);
            break;

        case 0xE0: /* pitch bend */
            fluid_synth_pitch_bend( synth, channel,
                                    ( data[ 1 ] << 7) | data[ 0 ]);
            break;

        case 0xF0:
            /* ignore SysEx events */
            break;
    }

    return decodeDelta( track );
}

/**
 * Decode OS/2 SysEx event
 *
 * @param[in] track Pointer to a track
 * @return 0 on success, -1 on error
 */
static int decodeOS2SysExEvent( PKMTRK track )
{
    PKMEMFD mfd = track->dec->mfd;
    uint8_t sysex[ 10 - 1 ]; /* F0 was consumed already */
    int i;

    /* SysEx event ends with 0xF7 */
    for( i = 0; i < sizeof( sysex ); i++ )
    {
        if( memRead( mfd, sysex + i, 1 ) == -1)
            return -1;

        track->offset++;

        if( sysex[ i ] == 0xF7 )
            break;
    }

    /* Ignore not supported SysEx event */
    if( i == sizeof( sysex ))
    {
        do
        {
            if( memRead( mfd, sysex, 1 ) == -1 )
                return -1;

            track->offset++;
        } while( sysex[ 0 ] != 0xF7 );
    }
    else if( memcmp( sysex, "\x00\x00\x3A", 3 ) == 0 )
    {
        uint8_t type = sysex[ 3 ] & 0x7F;

        if( type == 1 )         /* Timing Compression(Long) */
        {
            uint8_t ll = sysex[ 4 ] & 0x7F;
            uint8_t mm = sysex[ 5 ] & 0x7F;

            track->nextTick += mm << 7 | ll;
        }
        else if( type >= 7 )    /* Timing Compression(Short) */
            track->nextTick += type;
        else if( type == 3 )    /* Device Driver Control */
        {
            uint8_t cmd = sysex[ 4 ];

            if( cmd == 2 )      /* Tempo Control */
            {
                uint8_t tl = sysex[ 5 ] & 0x7F;
                uint8_t tm = sysex[ 6 ] & 0x7F;

                track->dec->tempo = 60 * 1000000 / (( tm << 7 | tl ) / 10 );
            }
        }
    }

    return 0;
}

/**
 * Decode OS/2 event
 *
 * @param[in] track Pointer to a track
 * @return 0 on success, -1 on error
 */
static int decodeOS2Event( PKMTRK track )
{
    PKMEMFD mfd = track->dec->mfd;
    fluid_synth_t *synth = track->dec->synth;

    uint8_t status;
    uint8_t event;
    uint8_t channel;

    if( track->offset >= track->length )
    {
        track->nextTick = END_OF_TRACK;

        return 0;
    }

    if( memRead( mfd, &status, 1 ) == -1 )
        return -1;
    track->offset++;

    /* implicit status ? */
    if( status < 0x80 )
    {
        status = track->status;
        if( memSeek( mfd, -1, KMDEC_SEEK_CUR ) == -1 )
            return -1;
        track->offset--;
    }

    if( status < 0x80 )
        return -1;

    if( status < 0xF0 )
        track->status = status;

    event   = status & 0xF0;
    channel = status & 0x0F;

    /* calculate length of event data */
    int len = 0;

    /*
     * status event 0x80, 0x90, 0xA0, 0xB0, 0xE0: len = 2
     * status event 0xC0, 0xD0: len = 1
     */
    switch( event )
    {
        case 0x80: case 0x90: case 0xA0: case 0xB0: case 0xE0:
            len = 2;
            break;

        case 0xC0: case 0xD0:
            len = 1;
            break;
    }

    uint8_t data[ len ];

    if( memRead( mfd, data, len ) == -1 )
        return -1;
    track->offset += len;

    data[ 0 ] &= 0x7F;
    data[ 1 ] &= 0x7F;

    /* pass MIDI event to fluidsynth */
    switch( event )
    {
        case 0x80:  /* note off */
            fluid_synth_noteoff( synth, channel, data[ 0 ]);
            break;

        case 0x90:  /* note on */
            fluid_synth_noteon( synth, channel, data[ 0 ], data[ 1 ]);
            break;

        case 0xA0: /* polyphonic aftertouch */
            /* not supported */
            break;

        case 0xB0:  /* control mode hnage */
            fluid_synth_cc( synth, channel, data[ 0 ], data[ 1 ]);
            break;

        case 0xC0:  /* program change */
            fluid_synth_program_change( synth, channel, data[ 0 ]);
            break;

        case 0xD0:  /* channel key pressure */
            fluid_synth_channel_pressure( synth, channel, data[ 0 ]);
            break;

        case 0xE0: /* pitch bend */
            fluid_synth_pitch_bend( synth, channel,
                                    ( data[ 1 ] << 7) | data[ 0 ]);
            break;

        case 0xF0:
            if( status == 0xF8 )
                track->nextTick++;
            else
                return decodeOS2SysExEvent( track );
            break;
    }

    return 0;
}

/**
 * Decode MIDI messages
 *
 * @param[in] track Pointer to a track
 * @param[in] mode Decode mode
 * @return 0 on success, -1 on error or on finished
 */
static int decode( PKMDEC dec, int mode )
{
    uint32_t nextTick = END_OF_TRACK;

    /* update next tick of a track if already decoded */
    for( int i = 0; i < dec->header.tracks; i++ )
    {
        PKMTRK track = dec->tracks + i;

        if( track->nextTick <= dec->tick )
        {
            if( track->dec->header.format == OS2MIDI )
            {
                if( decodeOS2Event( track ) == -1 )
                    return -1;
            }
            else if( decodeEvent( track ) == -1 )
                return -1;
        }

        if( nextTick > track->nextTick )
            nextTick = track->nextTick;
    }

    /* finished ? */
    if( nextTick == END_OF_TRACK )
        return -1;

    /* generate samples if new message was decoded every MIDI clock */
    if( nextTick > dec->tick )
    {
        int ticksPerSec = dec->header.division * CLOCK_BASE / dec->tempo;
        int delta = ticksPerSec * dec->clockUnit / CLOCK_BASE;

        /*
         * delta should be 1 at least. Otherwise tick does not progress
         * any more until tempo is changed to set delta to a value bigger
         * than 0.
         */
        if( delta == 0 )
            delta = 1;

        if( dec->tick + delta > nextTick )
            delta = nextTick - dec->tick;

        if( mode == DECODE_PLAY )
        {
            int samples = delta * dec->sampleRate / ticksPerSec;
            int len = samples * dec->sampleSize;

            free( dec->buffer );
            dec->buffer = malloc( len );
            if( !dec->buffer )
                return -1;

            dec->synth_write( dec->synth, samples,
                              dec->buffer, 0, 2, dec->buffer, 1, 2 );

            dec->bufLen = len;
            dec->bufPos = 0;
        }

        /* accumulate ticks */
        dec->tick += delta;

        /* accumulate clocks */
        dec->clock += CLOCK_BASE * delta / ticksPerSec;
    }

    return 0;
}

static int defaultOpen( const char *name )
{
    return open( name, O_RDONLY | O_BINARY );
}

static int defaultRead( int fd, void *buf, size_t n )
{
    return read( fd, buf, n );
}

static int defaultSeek( int fd, long offset, int origin )
{
    static int origins[] = { SEEK_SET, SEEK_CUR, SEEK_END };

    if( origin >= sizeof( origins ) / sizeof( origins[ 0 ]))
    {
        errno = EINVAL;
        return -1;
    }

    return lseek( fd, offset, origins[ origin ]);
}

static int defaultTell( int fd )
{
    return tell( fd );
}

static int defaultClose( int fd )
{
    return close( fd );
}

/**
 * Open decoder
 *
 * @param[in] fd File descriptor of a midi file
 * @param[in] sf2name Sound font file to open
 * @param[in] pkai Pointer to audio information
 * @param[in] closeFd Flag to indicate to close fd
 * @param[in] io IO functions to use
 * @return Decoder on success, NULL on error
 */
static
PKMDEC openEx( int fd, const char *sf2name, PKMDECAUDIOINFO pkai,
               bool closeFd, PKMDECIOFUNCS io)
{
    static KMDECIOFUNCS defaultIO = {
        .open = defaultOpen,
        .read = defaultRead,
        .seek = defaultSeek,
        .tell = defaultTell,
        .close = defaultClose,
    };

    PKMDEC dec;

    if( !io )
        io = &defaultIO;

    dec = calloc( 1, sizeof( *dec ));
    if( !dec )
        return NULL;

    /* init `sf' first in order to call kmdecClose() on failure */
    dec->sf = -1;

    dec->fd = fd;
    dec->closeFd = closeFd;

    dec->io = io;

    dec->mfd = memOpen( fd, io );
    if( !dec->mfd )
        goto fail;

    if( initMidiInfo( dec ) == -1 )
        goto fail;

    dec->settings = new_fluid_settings();
    if( !dec->settings )
        goto fail;

    dec->synth = new_fluid_synth( dec->settings );
    if( !dec->synth )
        goto fail;

    dec->sf = fluid_synth_sfload( dec->synth, sf2name, 1 );
    if( dec->sf == -1 )
        goto fail;

    char *sampleFormat;
    if( pkai->bps == KMDEC_BPS_S16 )
    {
        sampleFormat = "16bits";
        dec->synth_write = fluid_synth_write_s16;
    }
    else if( pkai->bps == KMDEC_BPS_FLOAT )
    {
        sampleFormat = "float";
        dec->synth_write = fluid_synth_write_float;
    }
    else
        goto fail;

    if( !fluid_settings_setstr( dec->settings, "audio.sample-format",
                                               sampleFormat )
        || !fluid_settings_setint( dec->settings, "synth.audio-channels",
                                   pkai->channels >> 1 )
        || !fluid_settings_setnum( dec->settings, "synth.sample-rate",
                                   pkai->sampleRate))
        goto fail;

    /* get clock unit from fluidsynth in ms */
    if( !fluid_settings_getint( dec->settings, "synth.min-note-length",
                                &dec->clockUnit ))
        dec->clockUnit = 10;
    dec->clockUnit *= CLOCK_BASE / 1000;    /* ms to us */

    dec->sampleRate = pkai->sampleRate;
    dec->sampleSize = pkai->channels * ( pkai->bps >> 3 );

    dec->tempo = DEFAULT_TEMPO;
    dec->numerator = DEFAULT_NUMERATOR;
    dec->denominator = DEFAULT_DENOMINATOR;

    /* calculate total samples */
    while( decode( dec, DECODE_SEEK ) != -1 )
        /* nothing */;

    dec->duration = dec->clock;

    /* reset decoder to intial status */
    if( reset( dec ) == -1 )
        goto fail;

    return dec;

fail:
    kmdecClose( dec );

    return NULL;
}

/**
 * Open decoder with a file name
 *
 * @param[in] name File name to open
 * @param[in] sf2name Sound font file to open
 * @param[in] pkai Pointer to audio information
 * @return Decoder on success, NULL on error
 */
PKMDEC kmdecOpen( const char *name, const char *sf2name, PKMDECAUDIOINFO pkai )
{
    return kmdecOpenEx( name, sf2name, pkai, NULL );
}

/**
 * Open decoder with a file name
 *
 * @param[in] name File name to open
 * @param[in] sf2name Sound font file to open
 * @param[in] pkai Pointer to audio information
 * @param[in] io Pointer to IO functions. If NULL, file IOs is used
 * @return Decoder on success, NULL on error
 */
PKMDEC kmdecOpenEx( const char *name, const char *sf2name,
                    PKMDECAUDIOINFO pkai, PKMDECIOFUNCS io )
{
    int fd;

    if( io )
        fd = io->open( name );
    else
        fd = defaultOpen( name );

    if( fd == -1)
        return NULL;

    return openEx( fd, sf2name, pkai, true, io );
}

/**
 * Open decoder with a file descriptor
 *
 * @param[in] fd File descriptor of a midi file
 * @param[in] sf2name Sound font file to open
 * @param[in] pkai Pointer to audio information
 * @return Decoder on success, NULL on error
 */
PKMDEC kmdecOpenFd( int fd, const char *sf2name, PKMDECAUDIOINFO pkai )
{
    return kmdecOpenFdEx( fd, sf2name, pkai, NULL );
}

/**
 * Open decoder with a file descriptor
 *
 * @param[in] fd File descriptor of a midi file
 * @param[in] sf2name Sound font file to open
 * @param[in] pkai Pointer to audio information
 * @param[in] io Pointer to IO functions. If NULL, file IOs is used
 * @return Decoder on success, NULL on error
 */
PKMDEC kmdecOpenFdEx( int fd, const char *sf2name,
                    PKMDECAUDIOINFO pkai, PKMDECIOFUNCS io )
{
    return openEx( fd, sf2name, pkai, false, io );
}

/**
 * Close decoder
 *
 * @param[in] dec Pointer to a deocder
 * @return 0 on success, -1 on error
 */
void kmdecClose( PKMDEC dec )
{
    if( !dec )
        return;

    free( dec->buffer );

    if( dec->sf != -1 )
        fluid_synth_sfunload( dec->synth, dec->sf, 1 );
    delete_fluid_synth( dec->synth );
    delete_fluid_settings( dec->settings );

    free( dec->tracks );

    memClose( dec->mfd );

    if( dec->closeFd )
        dec->io->close( dec->fd );

    free( dec );
}

/**
 * Fill the given buffer with decoded MIDI messages
 *
 * @param[in] dec Pointer to a deocder
 * @param[out] buffer where to store the decoded messages
 * @param[in] size Size of buffer in bytes
 * @return Size filled in buffer in bytes
 */
int kmdecDecode( PKMDEC dec, void *buffer, int size )
{
    if( !dec )
        return 0;

    int total = 0;

    while( size > 0 )
    {
        if( dec->bufLen == 0 && decode( dec, DECODE_PLAY ) == -1 )
            break;

        int len = MIN( size, dec->bufLen );
        memcpy( buffer, dec->buffer + dec->bufPos, len );

        buffer = ( char * )buffer + len;
        size -= len;

        dec->bufPos += len;
        dec->bufLen -= len;

        total += len;
    }

    return total;
}

/**
 * Get duration of MIDI file in milli-seconds
 *
 * @param[in] dec Pointer to a deocder
 * @return Length of MIDI in milli-seconds
 */
int kmdecGetDuration( PKMDEC dec )
{
    if( !dec )
        return -1;

    return 1000 * dec->duration / CLOCK_BASE;
}

/**
 * Get current position of decoder in milli-seconds
 *
 * @param[in] dec Pointer to a deocder
 * @return Current position of decoder in milli-seconds
 */
int kmdecGetPosition( PKMDEC dec )
{
    if( !dec )
        return -1;

    return 1000 * dec->clock / CLOCK_BASE;
}

/**
 * Seek to the given position
 *
 * @param[in] dec Pointer to a deocder
 * @param[in] offset Offset in ms
 * @param[in] origin Origin of seek
 * @return 0 on success, -1 on error
 */
int kmdecSeek( PKMDEC dec, int offset, int origin )
{
    if( !dec )
        return -1;

    uint64_t clock, originClock;

    switch( origin )
    {
        case KMDEC_SEEK_SET:
            originClock = 0;
            break;

        case KMDEC_SEEK_CUR:
            originClock = dec->clock;
            break;

        case KMDEC_SEEK_END:
            originClock = dec->duration;
            break;

        default:
            return -1;
    }

    clock = originClock + CLOCK_BASE * offset / 1000;
    /* wrapped around ? */
    if( offset < 0 && clock > originClock )
        clock = 0;
    else if( clock > dec->duration )
        clock = dec->duration;

    if( clock < dec->clock )
        reset( dec );

    while( dec->clock < clock && decode( dec, DECODE_SEEK ) != -1 )
        /* nothing */;

    if( dec->clock >= clock )
        return 0;

    return -1;
}

