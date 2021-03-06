/**
 * @brief Write notes to MIDI file
 * @file  midifile.cpp
 * Platform: Arduino UNO R3 using Arduino IDE
 * Documentation: http://www.coertvonk.com/technology/embedded/arduino-pitch-detector-13252
 *
 * GNU GENERAL PUBLIC LICENSE Version 3, check the file LICENSE for more information
 * (c) Copyright 2015-2016, Johan Vonk
 * All rights reserved.  Use of copyright notice does not imply publication.
 * All text above must be included in any redistribution
 **/

#include <Arduino.h>
#include <stdint.h>
#include <SD.h>

#include "config.h"
#include "debug.h"
#include "pitch.h"
#include "sample_t.h"
#include "segmentbuf.h"
#include "midi_t.h"
#include "midifile.h"

#if DST == DST_PIANOROLL

namespace {

    // work towards 1 tick per msec
    uint16_t const BEATS_PER_MIN = 125;
    uint16_t const TICKS_PER_BEAT = 1000 / BEATS_PER_MIN;

    // write bytes to file
    bool const                         // returns true on success, false otherwise
    _write( File & f,                  // file to append to
            void const * const  data,  // pointer to data
            size_t const dataLen )     // data length
    {
#if 0
        static uint_least8_t n = 0;

        for ( uint_least8_t ii = 0; ii < dataLen; ii++ ) {
            uint8_t const b = ((uint8_t *)data)[ii];
            Serial.print( b < 0x10 ? " 0" : " " );
            Serial.print( b, HEX );
            if ( ++n % 20 ) {
                n = 0;
                //Serial.println();
            }
        }
        Serial.println();
#endif
        return f.write( (uint8_t *)data, dataLen ) == dataLen;
    }


    // Write MIDI file header
    //   for example 4D 54 68 64 08 00 00 00 00 00 00 00 01 00 60 00
    //   where type="MHdr" len=8(0x00000008) format=0(0x00) tracks=1(0x0001) ticksPerQnote=(0x0060)
    bool const                // returns true on success, false otherwise
    _writeHeader( File & f )  // file to append to
    {
        midiHeader_t const header = {
            {{'M', 'T', 'h', 'd'}, sizeof( midiHeader_t ) - sizeof( midiChunkHdr_t )},
            {0, 1, TICKS_PER_BEAT}
        };
        return _write( f, &header, sizeof( header ) );
    }


    // Write MIDI track begin
    //   for example 4D 54 72 6B 03 00 00 00, where type="MTrk", len=3(0x00000003)
    bool const                              // returns true on success, false otherwise
    _writeTrackBegin( File &    f,          // file to append to
                      uint32_t  tracklen )  // #bytes to follow
    {

        midiChunkHdr_t const track = {{'M', 'T', 'r', 'k'}, tracklen};

        return _write( f, &track, sizeof( track ) );
    }


    // Write MIDI track tempo event
    //   for example FF 51 03 07 A1 20, where sysEx=meta(0xFF), tempo(0x51), len=3(0x03), tempo=0x20A107

    bool const                                // returns true on success, false otherwise
    _writeTrackTempo( File &         f,       // file to append to
                      uint32_t const tempo )  // tempo [usec/quaternote]
    {
        // 0xFF 0x51 0x03 >>16 >>8 >>0
        uint8_t const d1 = (tempo >> 16) & 0xFF;
        uint8_t const d2 = (tempo >> 8) & 0xFF;
        uint8_t const d3 = (tempo >> 0) & 0xFF;

        midiMeta_t const event = {midiSysExTag_t::meta, midiMetaType_t::setTempo, META_TEMPOCHANGE_LEN, {d1, d2, d3}};

        return _write( f, &event, 3 + META_TEMPOCHANGE_LEN );
    }


    // Write variable-length integer
    //   MIDI lengths are stored as variable length integers, between one and four bytes long.
    //   Each byte only uses the lower 7-bits, with the MSB set if there is another length byte following.
    //   The bytes are in big endian order.

    bool const                  // returns true on success, false otherwise
    _writeVarLen( File &   f,   // file to append to
                  uint32_t d )  // value to write
    {
        uint32_t buffer = d & 0x7F;

        while ( (d >>= 7) != 0 ) {
            buffer <<= 8;
            buffer |= ((d & 0x7F) | 0x80);
        }
        while ( true ) {
            size_t const len = sizeof( uint8_t );
            if ( _write( f, &buffer, len ) == false ) {
                return false;
            }
            if ( buffer & 0x80 ) {
                buffer >>= 8;
            } else {
                break;
            }
        }
        return true;
    }


    INLINE uint32_t const
    _msec2ticks( uint32_t const ms )
    {
        return (uint64_t)ms * BEATS_PER_MIN * TICKS_PER_BEAT / 60000;
    }


    // Write MIDI note event
    //   for example 00 19 3E 38, where delay=0(0x00), event=noteOn(0x19), pitch=0x3E, velocity=0x38, or
    //               81 01 18 3E 38, where delay=...(0x8101), event=noteOff(0x18), pitch=0x3E velocity=0x3B

    bool const                                         // returns true on success, false otherwise
    _writeTrackNote( File &                  f,        // file to append to
                     midiTime_t const        delay,    // delay compared to prior event (0 if none) [msec]
                     midiEvent_t const       eventNr,  // note event
                     segmentPitch_t const    pitch,    // note pitch
                     segmentEnergy_t const velocity )  // note velocity
    {
        _writeVarLen( f, _msec2ticks( delay ) );

        uint8_t const n1 = static_cast<uint8_t>(eventNr) & 0x0F;
        uint8_t const n2 = Config::MIDI_CHANNEL & 0x0F;

        midiEventNote_t event = {n1, n2, pitch, velocity};
        size_t const len = sizeof( event );

        return _write( f, &event, len );
    }


    // Write MIDI track end
    //   for example FF 2F 00, where sysEx=meta(0xFF), endoftrack(0x2F) len=0(0x00)

    bool const                  // returns true on success, false otherwise
    _writeTrackEnd( File & f )
    {
        midiMeta_t const end = {midiSysExTag_t::meta, midiMetaType_t::trackEnd, META_TRACKEND_LEN};  // 0xFF, 0x2F, 0x00

        return _write( f, &end, 3 + META_TRACKEND_LEN );
    }

}  // name space


int_least8_t const
MidiFile::begin( uint_least8_t const cs )
{
#if SRC != SRC_FILE  /* otherwise this is done already */
    if ( SD.begin( cs ) == false ) {  // declared in SD.cpp
        return -1;
    }
#endif
    return 0;
}


int_least8_t const
MidiFile::write( SegmentBuf * const segmentBuf,  // buffer with segmented notes
                 char const * const fname )      // file name to write to on SD card
{
    File f = SD.open( fname, FILE_WRITE );
    if ( !f ) {
        return -1;
    }

    // write header

    if ( _writeHeader( f ) == false ) {
        return -2;
    }

    // write track

    if ( _writeTrackBegin( f, META_TEMPOCHANGE_LEN + segmentBuf->len() * sizeof( midiEventNote_t ) ) == false ||
         _writeTrackTempo( f, 60000000UL / BEATS_PER_MIN ) == false ) {  // 60000000 usec/min
        return -3;
    }

    uint_least8_t ii = 0;
    while ( segment_t const * note = segmentBuf->tailPtr( ii++ ) ) {
        if ( _writeTrackNote( f, note->onset, midiEvent_t::noteOn, note->pitch, note->energy ) == false ||
             _writeTrackNote( f, note->duration, midiEvent_t::noteOff, note->pitch, note->energy ) == false ) {
            return -4;
        }
    }

    if ( _writeTrackEnd( f ) == false ) {
        return -5;
    }

    f.close();
    return 0;
}
#endif
