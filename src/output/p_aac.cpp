/*
 * mkvmerge -- utility for splicing together matroska files
 * from component media subtypes
 *
 * Distributed under the GPL
 * see the file COPYING for details
 * or visit http://www.gnu.org/copyleft/gpl.html
 *
 * $Id$
 *
 * AAC output module
 *
 * Written by Moritz Bunkus <moritz@bunkus.org>.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "pr_generic.h"
#include "aac_common.h"
#include "p_aac.h"
#include "matroska.h"
#include "mkvmerge.h"

using namespace libmatroska;

aac_packetizer_c::aac_packetizer_c(generic_reader_c *nreader,
                                   int nid,
                                   int nprofile,
                                   unsigned long nsamples_per_sec,
                                   int nchannels, 
                                   track_info_c *nti,
                                   bool nemphasis_present,
                                   bool nheaderless)
  throw (error_c):
  generic_packetizer_c(nreader, nti) {
  packetno = 0;
  bytes_output = 0;
  samples_per_sec = nsamples_per_sec;
  channels = nchannels;
  id = nid;
  profile = nprofile;
  headerless = nheaderless;
  emphasis_present = nemphasis_present;
  num_packets_same_tc = 0;
  last_timecode = -1;

  set_track_type(track_audio);
  set_track_default_duration((int64_t)(1024 * 1000000000.0 * ti->async.linear /
                                       samples_per_sec));
}

aac_packetizer_c::~aac_packetizer_c() {
}

unsigned char *
aac_packetizer_c::get_aac_packet(unsigned long *header,
                                 aac_header_t *aacheader) {
  int pos, i, up_shift, down_shift, size;
  unsigned char *buf, *src, *packet_buffer;
  double pins;

  packet_buffer = byte_buffer.get_buffer();
  size = byte_buffer.get_size();
  pos = find_aac_header(packet_buffer, size, aacheader, emphasis_present);
  if (pos < 0)
    return NULL;
  if ((pos + aacheader->bytes) > size)
    return NULL;

  pins = 1024 * 1000000000.0 / samples_per_sec;

  if (needs_negative_displacement(pins)) {
    /*
     * AAC audio synchronization. displacement < 0 means skipping an
     * appropriate number of packets at the beginning.
     */
    displace(-pins);
    byte_buffer.remove(pos + aacheader->bytes);

    return NULL;
  }

  if (verbose && (pos > 0))
    mxwarn("aac_packetizer: skipping %d bytes (no valid AAC header "
           "found). This might make audio/video go out of sync, but this "
           "stream is damaged.\n", pos);
  if ((aacheader->header_bit_size % 8) == 0)
    buf = (unsigned char *)safememdup(packet_buffer + pos +
                                      aacheader->header_byte_size,
                                      aacheader->data_byte_size);
  else {
    // Header is not byte aligned, i.e. MPEG-4 ADTS
    // This code is from mpeg4ip/server/mp4creator/aac.cpp
    buf = (unsigned char *)safemalloc(aacheader->data_byte_size);

    up_shift = aacheader->header_bit_size % 8;
    down_shift = 8 - up_shift;
    src = packet_buffer + pos + aacheader->header_bit_size / 8;

    buf[0] = src[0] << up_shift;
    for (i = 1; i < aacheader->data_byte_size; i++) {
      buf[i - 1] |= (src[i] >> down_shift);
      buf[i] = (src[i] << up_shift);
    }
  }

  if (needs_positive_displacement(pins)) {
    /*
     * AAC audio synchronization. displacement > 0 is solved by duplicating
     * the very first AAC packet as often as necessary. I cannot create
     * a packet with total silence because I don't know how, and simply
     * settings the packet's values to 0 does not work as the AAC header
     * contains a CRC of its data.
     */
    displace(pins);

    return buf;
  }

  byte_buffer.remove(pos + aacheader->bytes);

  return buf;
}

void
aac_packetizer_c::set_headers() {
  if (id == AAC_ID_MPEG4) {
    if (profile == AAC_PROFILE_MAIN)
      set_codec_id(MKV_A_AAC_4MAIN);
    else if (profile == AAC_PROFILE_LC)
      set_codec_id(MKV_A_AAC_4LC);
    else if (profile == AAC_PROFILE_SSR)
      set_codec_id(MKV_A_AAC_4SSR);
    else if (profile == AAC_PROFILE_LTP)
      set_codec_id(MKV_A_AAC_4LTP);
    else if (profile == AAC_PROFILE_SBR)
      set_codec_id(MKV_A_AAC_4SBR);
    else
      die("aac_packetizer: Unknown AAC MPEG-4 object type %d.", profile);
  } else {
    if (profile == AAC_PROFILE_MAIN)
      set_codec_id(MKV_A_AAC_2MAIN);
    else if (profile == AAC_PROFILE_LC)
      set_codec_id(MKV_A_AAC_2LC);
    else if (profile == AAC_PROFILE_SSR)
      set_codec_id(MKV_A_AAC_2SSR);
    else if (profile == AAC_PROFILE_SBR)
      set_codec_id(MKV_A_AAC_2SBR);
    else
      die("aac_packetizer: Unknown AAC MPEG-2 profile %d.", profile);
  }
  set_audio_sampling_freq((float)samples_per_sec);
  set_audio_channels(channels);

  generic_packetizer_c::set_headers();
}

int
aac_packetizer_c::process(memory_c &mem,
                          int64_t timecode,
                          int64_t,
                          int64_t,
                          int64_t) {
  unsigned char *packet;
  unsigned long header;
  aac_header_t aacheader;
  int64_t my_timecode, duration;

  debug_enter("aac_packetizer_c::process");

  my_timecode = 0;
  if (headerless) {
    if (timecode != -1) {
      my_timecode = timecode;
      if (last_timecode == timecode) {
        num_packets_same_tc++;
        my_timecode +=
          (int64_t)(num_packets_same_tc * 1024 * 1000000000.0 /
                    samples_per_sec);
      } else {
        last_timecode = timecode;
        num_packets_same_tc = 0;
      }
    } else
      my_timecode = (int64_t)(1024 * 1000000000.0 * packetno /
                              samples_per_sec);
    duration = (int64_t)(1024 * 1000000000.0 * ti->async.linear /
                         samples_per_sec);
    packetno++;

    if (needs_negative_displacement(duration)) {
      displace(-duration);
      return EMOREDATA;
    }
    while (needs_positive_displacement(duration)) {
      add_packet(mem, my_timecode + ti->async.displacement, duration);
      displace(duration);
    }
        
    my_timecode = (int64_t)((my_timecode + ti->async.displacement) *
                            ti->async.linear);
    mxverb(2, "aac: my_tc = %lld\n", my_timecode);
    add_packet(mem, my_timecode, duration);

    debug_leave("aac_packetizer_c::process");

    return EMOREDATA;
  }

  byte_buffer.add(mem.data, mem.size);
  while ((packet = get_aac_packet(&header, &aacheader)) != NULL) {
    if (timecode == -1)
      my_timecode = (int64_t)(1024 * 1000000000.0 * packetno /
                              samples_per_sec);
    else
      my_timecode = timecode + ti->async.displacement;
    my_timecode = (int64_t)(my_timecode * ti->async.linear);
    memory_c mem(packet, aacheader.data_byte_size, true);
    add_packet(mem, my_timecode,
               (int64_t)(1024 * 1000000000.0 *
                         ti->async.linear / samples_per_sec));
    packetno++;
  }

  debug_leave("aac_packetizer_c::process");

  return EMOREDATA;
}

void
aac_packetizer_c::dump_debug_info() {
  mxdebug("aac_packetizer_c: queue: %d; buffer size: %d\n",
          packet_queue.size(), byte_buffer.get_size());
}

int
aac_packetizer_c::can_connect_to(generic_packetizer_c *src) {
  aac_packetizer_c *asrc;

  asrc = dynamic_cast<aac_packetizer_c *>(src);
  if (asrc == NULL)
    return CAN_CONNECT_NO_FORMAT;
  if ((samples_per_sec != asrc->samples_per_sec) ||
      (channels != asrc->channels) || (id != asrc->id) ||
      (profile != asrc->profile))
    return CAN_CONNECT_NO_PARAMETERS;
  return CAN_CONNECT_YES;
}

