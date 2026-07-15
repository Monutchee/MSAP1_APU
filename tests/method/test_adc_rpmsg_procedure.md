# AD7771 ADC and RPMsg Test Procedure

## Purpose

Use this procedure to verify the MSAP1 AD7771 control, capture, and APU
visualization paths. It also reproduces the current high-rate RPMsg starvation
problem without confusing the requested display rate with the ADC capture rate.

The RPU always captures all eight ADC channels at 32,000 frames/s. The
`adc-view --rate` option controls only the decimated copy sent to Linux for
display; it does not reconfigure the AD7771.

## Safety and prerequisites

- Do not connect the sensor board to the electrical grid for this test.
- Boot the matching PL bitstream, R5 core 0 firmware, and Linux image.
- Confirm that the RPU heartbeat LED is blinking before starting.
- Run the commands as `root` on the MSAP1 Linux target.
- Reboot the target before repeating a high-rate stress test if RPMsg stops
  responding.

Record the PL, RPU, APU, and Yocto revisions used for the test.

## Command summary

Query the ADC control and capture health:

```sh
msap1-apu-app adc-health
```

Display only the three voltage channels for a fixed duration:

```sh
msap1-apu-app adc-view --rate RATE_HZ --channels "4,5,6" --duration SECONDS
```

Useful `adc-view` options are:

| Option | Meaning |
| --- | --- |
| `--rate HZ` | Decimated visualization rate; it must divide 32,000 exactly. |
| `--channels LIST` | Comma-separated ADC channels in the range 0 to 7. |
| `--duration SECONDS` | Stop after the requested elapsed time. |
| `--frames COUNT` | Stop after the requested number of displayed frames. |
| `--format terminal\|table\|csv\|jsonl` | Select the output format. |
| `--output FILE` | Write the selected format to a file. |
| `--timeout-ms MS` | Set the initial reply/data timeout. |

## 1. Baseline health check

Run:

```sh
msap1-apu-app adc-health
```

Expected control and capture results:

- `SPI responsive`, `ADC initialized`, `INIT_COMPLETE`, `Configuration match`,
  and `Capture active` are `yes`.
- `SPI error` is `none`.
- `Sample rate` is `32000 frame/s` and `Expected decimation` is `64`.
- `DMA packets` and `Frames` increase between repeated checks.
- `FIFO overflows` remains `0` before a high-rate stress test.

Current known limitation:

- Overall health reports `FAIL` because `Header errors` is currently 100%.
- `Capture flags` commonly reads `0x0000006b`.
- The PL DOUT parser is not aligned correctly yet. Until that is fixed, the
  displayed samples and `ADC alerts` counter must not be treated as valid ADC
  measurements.

## 2. Low-rate functional test

Run:

```sh
msap1-apu-app adc-view --rate 20 --channels "4,5,6" --duration 10
```

At 20 displayed frames/s, the RPU selects one frame for every 1,600 captured
frames. This produces approximately 20 RPMsg sample messages/s.

Expected result:

- The viewer starts, displays channels 4, 5, and 6, and exits normally.
- A 10-second run should display approximately 200 frames. Record the result
  if the count differs materially from 200.
- The heartbeat LED continues blinking during and after the test.
- No RPMsg acknowledgement timeout is printed.
- The following command can be run immediately and receives a response:

  ```sh
  msap1-apu-app adc-health
  ```

- `FIFO overflows` remains `0`.
- Capture flag bits 2 (`FIFO full`) and 4 (`FIFO overflow sticky`) remain clear;
  the currently observed value `0x0000006b` satisfies this condition.

This is the current functional-test rate for repeated ADC/RPMsg testing.

## 3. High-rate stress test

This test documents a known failure. Run it only when a target reboot is
acceptable.

```sh
msap1-apu-app adc-view --rate 1000 --channels "4,5,6" --duration 10
```

At 1,000 displayed frames/s, each 256-frame DMA packet contains eight selected
frames. The wire format holds at most six frames per RPMsg message, so the RPU
sends two messages per DMA packet, or approximately 250 messages/s.

Current expected failure symptoms can include:

- The heartbeat LED stops blinking.
- Capture flags change to `0x00000077`, which has both `FIFO full` and `FIFO
  overflow sticky` set.
- The viewer times out waiting for the STOP acknowledgement.
- A subsequent `adc-health` or `adc-view` command times out waiting for an RPU
  acknowledgement.

These symptoms indicate that the priority-3 ADC task is spending too much time
performing blocking RPMsg sends. The priority-2 control task and priority-1 LED
task then cannot run reliably. They do not indicate that the AD7771 sampling
rate changed or that OpenAMP buffers were permanently consumed.

Reboot the target after this failure before continuing other tests.

## Rate comparison

| Property | Low-rate functional test | High-rate stress test |
| --- | ---: | ---: |
| ADC capture rate | 32,000 frames/s | 32,000 frames/s |
| Requested display rate | 20 frames/s | 1,000 frames/s |
| Decimation stride | 1,600 captured frames | 32 captured frames |
| Selected frames per 256-frame DMA packet | 0.16 average | 8 |
| Approximate RPMsg sample messages/s | 20 | 250 |
| Current expected behavior | Stable | RPU task starvation possible |

## Capture flag reference

The low ten bits of `Capture flags` are:

| Bit | Meaning |
| ---: | --- |
| 0 | Capture enabled |
| 1 | Receiver busy |
| 2 | FIFO full |
| 3 | FIFO empty |
| 4 | FIFO overflow sticky |
| 5 | Header error sticky |
| 6 | ADC alert/header alert sticky |
| 7 | `ADC_DRDY_N` input level |
| 8 | FIFO write reset busy |
| 9 | FIFO read reset busy |

For transport testing, bits 2 and 4 are the important overload indicators.
Bits 5 and 6 are expected to remain set until the separate DOUT parser alignment
problem is fixed.

## Test record

Record at least:

- Test date and operator.
- PL, RPU, APU, and Yocto revisions.
- Exact command used.
- Whether the heartbeat LED continued blinking.
- Viewer displayed-frame and message counts.
- Health output before and after the viewer run.
- Any timeout, FIFO-full, or overflow indication.
