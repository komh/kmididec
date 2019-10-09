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

/** @file kmididec.h */

#ifndef KMIDIDEC_H
#define KMIDIDEC_H

#ifdef __cplusplus
extern "C" {
#endif

/* KMIDIDEC version macro */
#define KMIDIDEC_VERSION    "0.1.0"

/**
 * @defgroup kmdecbps BPS
 * {
 */
#define KMDEC_BPS_S16   16
#define KMDEC_BPS_FLOAT 32
/** @} */

/**
 * @defgroup kmdecseekmodes Seek modes
 * {
 */
#define KMDEC_SEEK_SET  0   /**< from the beginning */
#define KMDEC_SEEK_CUR  1   /**< from current position */
#define KMDEC_SEEK_END  2   /**< from the end */
/** @} */

/**
 * Audio information
 */
typedef struct kmdecaudioinfo
{
    int bps;        /**< bits per sample */
    int channels;   /**< a number of channels */
    int sampleRate; /**< samples per second */
} KMDECAUDIOINFO, *PKMDECAUDIOINFO;

typedef struct kmdec *PKMDEC;

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
                  PKMDECAUDIOINFO pkai );

/**
 * Close decoder
 *
 * @param[in] dec Pointer to a deocder
 * @return 0 on success, -1 on error
 */
void kmdecClose( PKMDEC dec );

/**
 * Fill the given buffer with decoded MIDI messages
 *
 * @param[in] dec Pointer to a deocder
 * @param[out] buffer where to store the decoded messages
 * @param[in] size Size of buffer in bytes
 * @return Size filled in buffer in bytes
 */
int kmdecDecode( PKMDEC dec, void *buffer, int size );

/**
 * Get length of MIDI in milli-seconds
 *
 * @param[in] dec Pointer to a deocder
 * @return Length of MIDI in milli-seconds
 */
int kmdecGetDuration( PKMDEC dec );

/**
 * Get current position of decoder in milli-seconds
 *
 * @param[in] dec Pointer to a deocder
 * @return Current position of decoder in milli-seconds
 */
int kmdecGetPosition( PKMDEC dec );

/**
 * Seek to the given position
 *
 * @param[in] dec Pointer to a deocder
 * @param[in] offset Offset in ms
 * @param[in] origin Origin of seek
 * @return 0 on success, -1 on error
 */
int kmdecSeek( PKMDEC dec, int offset, int origin );

#ifdef __cplusplus
}
#endif

#endif
