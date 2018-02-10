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
#include <sys/param.h>
#include <io.h>
#include <fcntl.h>

#include <fluidsynth.h>
/* missed API declaration in 1.0.9 */
FLUIDSYNTH_API
int fluid_synth_channel_pressure( fluid_synth_t *synth, int chan, int val );

#include "kmididec.h"

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

/* defaul values */
#define DEFAULT_TEMPO       500000 /* us/qn */
#define DEFAULT_NUMERATOR   4
#define DEFAULT_DENOMIRATOR 4

/* clocks per sec */
#define CLOCK_BASE  INT64_C( 1000000 ) /* us */

/**
 * MIDI decoder
 */
typedef struct kmdec
{
    int fd; /**< fd for MIDI file */

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
    uint8_t denomirator;    /**< denomirator */

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

static int initMidiInfo( PKMDEC dec );
static int reset( PKMDEC dec );

static int readVarQ( PKMTRK track, int *val );
static int decodeDelta( PKMTRK track);
static int decodeMetaEvent( PKMTRK track);
static int decodeEvent( PKMTRK track);
static int decode( PKMDEC dec, int mode );

/**
 * Initialize header and track information
 *
 * @param[in] dec Pointer to decoder
 * @return 0 on success, -1 on error
 */
static int initMidiInfo( PKMDEC dec )
{
    int fd = dec->fd;
    PKMTHD header = &dec->header;
    PKMTRK *tracks = &dec->tracks;
    uint8_t data[ 14 ];

    if( read( fd, data, sizeof( data )) == -1 )
        return -1;

    if( memcmp( data, "MThd\x00\x00\x00\x06", 8 ) != 0 )
    {
        fprintf( stderr, "Not supported MIDI file\n");

        return -1;
    }

    header->format = ntohs( *( uint16_t * )( data + 8 ));
    header->tracks = ntohs( *( uint16_t * )( data + 10 ));
    header->division = ntohs( *( uint16_t * )( data + 12 ));

    *tracks = calloc( header->tracks, sizeof( **tracks ));
    if( !tracks )
        return -1;

    for( int i = 0; i < header->tracks; i++ )
    {
        PKMTRK track = ( *tracks ) + i;
        if( read( fd, data, 8 ) == -1 || memcmp( data, "MTrk", 4 ) != 0 )
        {
fail:
            free( *tracks );
            *tracks = NULL;

            return -1;
        }

        track->dec = dec;
        track->start = tell( fd );
        track->length = ntohl( *( long * )( data + 4 ));
        if( decodeDelta( track ) == -1 )
            goto fail;

        if( lseek( fd, track->start + track->length, SEEK_SET ) == -1 )
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
        if( lseek( dec->fd, track->start, SEEK_SET ) == -1 ||
            decodeDelta( track ) == -1 )
            return -1;
        track->status = 0;
    }

    /* reset fluidsynth */
    fluid_synth_system_reset( dec->synth );

    /* reset to default values */
    dec->tempo = DEFAULT_TEMPO;
    dec->numerator = DEFAULT_NUMERATOR;
    dec->denomirator = DEFAULT_DENOMIRATOR;

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
    int fd = track->dec->fd;
    uint8_t b;
    int count = 0;

    int vq = 0;

    do
    {
        if( count >= 4 || read( fd, &b, 1 ) == -1 )
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
    int fd = track->dec->fd;
    uint8_t type;
    int len;

    if( track->offset >= track->length )
        return 0;

    /* type */
    if( read( fd, &type, 1 ) == -1 )
        return -1;
    track->offset++;

    /* length */
    if( readVarQ( track, &len ) == -1 )
        return -1;

    uint8_t data[ len + 1 ];
    data[ len ] = '\0';

    if( read( fd, data, len ) == -1 )
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

        case 0x54: /* SMTPE offset */
            if( len != 5 )
                return -1;
            break;

        case 0x58: /* time signature */
        {
            if( len != 4 )
                return -1;

            PKMDEC dec = track->dec;

            dec->numerator = data[ 0 ];
            dec->denomirator = 1 << data[ 1 ]; /* power of 2 */
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
    int fd = track->dec->fd;
    fluid_synth_t *synth = track->dec->synth;

    uint8_t status;
    uint8_t event;
    uint8_t channel;
    int len;

    if( track->offset >= track->length )
        return 0;

    if( lseek( fd, track->start + track->offset, SEEK_SET ) == -1 )
        return -1;

    if( read( fd, &status, 1 ) == -1 )
        return -1;
    track->offset++;

    /* implicit status ? */
    if( status < 0x80 )
    {
        status = track->status;
        if( lseek( fd, -1, SEEK_CUR ) == -1 )
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

    if( read( fd, data, len ) == -1 )
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
            if( decodeEvent( track ) == -1 )
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

        if( dec->tick + delta > nextTick )
            delta = nextTick - dec->tick;

        int samples = delta * dec->sampleRate / ticksPerSec;

        if( samples > 0 && mode == DECODE_PLAY )
        {
            dec->synth_write( dec->synth, samples,
                              dec->buffer, 0, 2, dec->buffer, 1, 2 );
        }

        dec->bufLen = samples * dec->sampleSize;
        dec->bufPos = 0;

        /* accumulate ticks */
        dec->tick += delta;

        /* accumulate clocks */
        dec->clock += CLOCK_BASE * delta / ticksPerSec;
        return dec->bufLen;
    }

    return 0;
}

/**
 * Open decoder
 *
 * @param[in] name File name to open
 * @param[in] sf2name Sound font file to open
 * @param[in] pkai Pointer to audio information
 * @return 0 on success, -1 on error
 * @remark Support only 16bits sample
 */
PKMDEC kmdecOpen( const char *name, const char *sf2name,
                  PKMDECAUDIOINFO pkai )
{
    PKMDEC dec;

    dec = calloc( 1, sizeof( *dec ));
    if( !dec )
        return NULL;

    /* init `sf' first in order to call kmdecClose() on failure */
    dec->sf = -1;

    dec->fd = open( name, O_RDONLY | O_BINARY );
    if( dec->fd == -1 || initMidiInfo( dec ) == -1 )
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
    dec->buffer = calloc( 1, dec->sampleRate * dec->clockUnit / CLOCK_BASE *
                             dec->sampleSize +
                             dec->sampleSize /* remainder + margin */);
    if( !dec->buffer )
        goto fail;

    dec->tempo = DEFAULT_TEMPO;
    dec->numerator = DEFAULT_NUMERATOR;
    dec->denomirator = DEFAULT_DENOMIRATOR;

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
    close( dec->fd );

    free( dec );
}

/**
 * Fill the given buffer with decoded MIDI messages
 *
 * @param[in] dec Pointer to a deocder
 * @param[out] buffer where to store the decoded messages
 * @param[in] size Size of buffer
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
        case SEEK_SET:
            originClock = 0;
            break;

        case SEEK_CUR:
            originClock = dec->clock;
            break;

        case SEEK_END:
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

