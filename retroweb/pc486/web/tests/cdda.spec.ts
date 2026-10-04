import { test, expect } from "./fixtures";

// CD-DA (Red Book audio) playback through the ATAPI CD-ROM drive on the
// secondary channel (0x170-0x177/0x376) -- PLAY AUDIO(10) and the stream it
// produces, mirroring atapi_cdrom_test.cpp's register-level fixture but
// driven through the real port block the way a DOS CD driver would. Mounts a
// small synthetic mixed-mode CUE+BIN disc (one data sector, one short audio
// track with a known waveform) and confirms cdromDrainSamples() reproduces
// that exact waveform at the real 44.1kHz rate, and that playback stops on
// its own at the end of the requested range -- the same contract
// atapi_cdrom_test.cpp verifies natively, exercised here through the actual
// embind/port surface the front end uses.
test.describe("ATAPI CD-DA audio playback", () => {
  test("PLAY AUDIO(10) streams the mounted track's real PCM samples", async ({
    livePage: page,
  }) => {
    const result = await page.evaluate(() => {
      const m = (window as any).__test.machine;

      // Track 1: one MODE1/2048 data sector (so this exercises the mixed-mode
      // path a real game CD uses, not just a lone audio track). Track 2: a
      // short CD-DA track -- a 4-frame repeating square wave, easy to verify
      // exactly rather than by spectral analysis. Left and right are inverted
      // from each other so a channel swap would also be caught.
      const dataBlocks = 1;
      const framesPerLba = 588;
      const audioLbas = 2;
      const audioFrames = audioLbas * framesPerLba;
      const amp = 12000;
      const bin = new Uint8Array(dataBlocks * 2048 + audioFrames * 4);
      let off = dataBlocks * 2048;
      for (let i = 0; i < audioFrames; i++) {
        const l = i % 4 < 2 ? amp : -amp;
        const r = -l;
        bin[off++] = l & 0xff; bin[off++] = (l >> 8) & 0xff;
        bin[off++] = r & 0xff; bin[off++] = (r >> 8) & 0xff;
      }
      // Track 2's INDEX 01 is in frames (75/sec, Red Book), which this
      // single-digit-second disc uses as a plain LBA counter: frame 1 is
      // right after track 1's one data sector.
      const cue =
        'FILE "disc.bin" BINARY\n' +
        "  TRACK 01 MODE1/2048\n" +
        "    INDEX 01 00:00:00\n" +
        "  TRACK 02 AUDIO\n" +
        "    INDEX 01 00:00:01\n";
      const mounted = m.mountCdromCue(cue, bin);

      // Drive the secondary ATA channel's real registers -- same protocol
      // atapi_cdrom_test.cpp's SendPacket/DrainData helpers use natively.
      const rd = (port: number) => m.portIn(port);
      const wr = (port: number, v: number) => m.portOut(port, v);
      const busy = () => (rd(0x376) & 0x80) !== 0;
      const drq = () => (rd(0x376) & 0x08) !== 0;

      function runToIdle() {
        for (let i = 0; i < 2000 && busy(); i++) m.runCycles(2000);
      }
      function sendPacket(cdb: number[]) {
        const full = cdb.slice();
        while (full.length < 12) full.push(0);
        wr(0x174, 0xfe); wr(0x175, 0xff);  // byte-count limit
        wr(0x171, 0x00);                    // features: PIO, no overlap
        wr(0x177, 0xa0);                    // PACKET
        for (const b of full) wr(0x170, b);
        runToIdle();
      }
      function drainData() {
        for (let guard = 0; guard < 100 && drq(); guard++) {
          const count = rd(0x174) | (rd(0x175) << 8);
          for (let i = 0; i < count; i++) rd(0x170);
        }
        runToIdle();
      }

      wr(0x176, 0xa0);                       // select device 0
      sendPacket([0x00]);                     // TEST UNIT READY -- surfaces the mount's unit attention
      sendPacket([0x03, 0, 0, 0, 18]);        // REQUEST SENSE
      drainData();                            // ...and clears it

      // PLAY AUDIO(10): starting LBA 1 (the audio track), for audioLbas LBAs.
      sendPacket([0x45, 0, 0, 0, 0, 1, 0, 0, audioLbas]);
      const playingRightAfter = m.cdromPlayingAudio();

      // Run enough real CPU cycles for every frame of the track to be
      // produced at 44.1kHz, with slack so the end-of-range stop also fires.
      // runCycles() can yield early mid-call (a host-side soft-lock guard),
      // so loop on the actual cycle delta rather than assuming one call
      // covers the whole budget.
      const cyclesPerFrame = m.cpuHz() / 44100;
      let remaining = Math.ceil(cyclesPerFrame * (audioFrames + 100));
      while (remaining > 0) {
        const before = m.totalCycles();
        m.runCycles(Math.min(remaining, 200000));
        const got = m.totalCycles() - before;
        if (got <= 0) break;
        remaining -= got;
      }

      const s = m.cdromDrainSamples();
      return {
        mounted,
        playingRightAfter,
        sampleRate: m.cdromSampleRateHz(),
        count: s.left.length,
        firstLeft: s.left.length ? s.left[0] : null,
        firstRight: s.right.length ? s.right[0] : null,
        fifthLeft: s.left.length > 4 ? s.left[4] : null,
        stillPlaying: m.cdromPlayingAudio(),
      };
    });

    expect(result.mounted).toBe(true);
    expect(result.playingRightAfter).toBe(true);
    expect(result.sampleRate).toBe(44100);
    // 2 LBAs * 588 frames/LBA, allowing slack for burst granularity.
    expect(result.count).toBeGreaterThan(1100);
    expect(result.count).toBeLessThanOrEqual(1176);
    expect(result.firstLeft).toBe(12000);
    expect(result.firstRight).toBe(-12000);
    expect(result.fifthLeft).toBe(12000);  // frame 4: back to the start of the square wave's cycle
    expect(result.stillPlaying).toBe(false);  // ran past the requested range and stopped on its own

    await page.evaluate(() => (window as any).__test.machine.ejectCdrom());
  });
});
