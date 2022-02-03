/* wwvb_dec
 * Copyright Peter Newton, 2022
 * License (SPDX code): BSD-2-Clause
 *
 * This program decodes the time signal from station WWVB in the USA.  It
 * requires a GPIO connection to a receiver that outputs the current
 * carrier level as a zero or one.  Example: Canaduino 60 kHz Atomic Clock
 * Receiver Module sold by universal-solder.ca.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>

#include <pigpio.h>

/* GPIO4 is pin 7 on Raspberry PI Zero */
#define GPIO 4

/* Choose SAMP_PERIOD to evenly divide 200, 500, and 800 */
#define SAMP_PERIOD 25
#define SAMP_PERIOD_USEC (1000*SAMP_PERIOD)
#define BUF_LEN_IN_SEC 120
#define SAMPLES_PER_SEC (1000/SAMP_PERIOD)
#define BLEN (SAMPLES_PER_SEC*BUF_LEN_IN_SEC)

#define DECODE_FAILURE (9999)

typedef struct {
  uint32_t bit;
  uint32_t weight;
} code_t;

/* The frame is made up of 60 bits (or markers).  These "codes" show which bits
 * make up a field.  See Wikipedia on WWVB.  Each bit takes one second in the
 * frame. */

code_t minutes_code[] = {{1, 40}, {2, 20}, {3, 10}, {5, 8}, {6, 4}, {7, 2}, {8, 1}};
code_t hours_code[] = {{12, 20}, {13, 10}, {15, 8}, {16, 4}, {17, 2}, {18, 1}};
code_t day_code[] = {{22, 200}, {23, 100}, {25, 80}, {26, 40}, {27, 20}, {28, 10},
		      {30, 8}, {31, 4}, {32, 2}, {33, 1}};
code_t year_code[] = {{45, 80}, {46, 40}, {47, 20}, {48, 10}, {50, 8}, {51, 4},
		      {52, 2}, {53, 1}};
code_t lyi_code[] = {{55, 1}};
code_t lsw_code[] = {{56, 1}};
code_t dst_code[] = {{57, 2}, {58, 1}};

typedef struct {
  char *name;
  uint32_t value;
  uint32_t score;
  uint32_t worst_score;
  uint32_t val_width;
  code_t *code;
  uint32_t code_len;
} field_t;

/* A frame contains fields, organized into this array */

field_t frame[] = {
  {"hours",  0xffffffff, 0xffffffff, 0xffffffff, 2, hours_code, sizeof(hours_code)/sizeof(hours_code[0])},
  {"minutes", 0xffffffff, 0xffffffff, 0xffffffff, 2, minutes_code, sizeof(minutes_code)/sizeof(minutes_code[0])},
  {"day",  0xffffffff, 0xffffffff, 0xffffffff, 3, day_code, sizeof(day_code)/sizeof(day_code[0])},
  {"year",  0xffffffff, 0xffffffff, 0xffffffff, 2, year_code, sizeof(year_code)/sizeof(year_code[0])},
  {"lyi",  0xffffffff, 0xffffffff, 0xffffffff, 1, lyi_code, sizeof(lyi_code)/sizeof(lyi_code[0])},
  {"lsw",  0xffffffff, 0xffffffff, 0xffffffff, 1, lsw_code, sizeof (lsw_code)/sizeof (lsw_code[0])},
  {"dst",  0xffffffff, 0xffffffff, 0xffffffff, 2, dst_code, sizeof(dst_code)/sizeof(dst_code[0])}
};

/* Indices for above */
#define HOURS 0
#define MINUTES 1
#define DAYNUM 2
#define YEAR 3
#define LYI 4
#define LSW 5
#define DST 6


/* circular buffer of sampled bits from receiver */
uint8_t bits[BLEN];

/* Read bits from a file for offline processing */

void fill_buffer_file(char *fname)
{
  FILE *fp;

  if ((fp = fopen(fname, "rb")) == NULL) {
    fprintf(stderr, "Error: could not open file %s for reading\n", fname);
    exit(EXIT_FAILURE);
  }

  if (fread(bits, sizeof(bits[0]), sizeof(bits)/sizeof(bits[0]), fp) < 60*SAMPLES_PER_SEC)
    fprintf(stderr, "Warning: input file likely too short\n");

  fclose(fp);
}

/* Save the buffer of bits for later offline processng */

void save_buffer_file(char *fname)
{
  FILE * fp;

  if ((fp = fopen(fname, "wb")) == NULL) {
    fprintf(stderr, "Warning: could not open file %s for writing\n", fname);
  return;
}

  fwrite(bits, 1, sizeof(bits), fp);
  fclose(fp);
}


/* Fill the buffer of bits by sampling the GPIO.  This could be senstive to the
 * accuracy and jitter of gpioTick. */

void fill_buffer_gpio(void)
{
  uint32_t i, first_tick;

  first_tick = gpioTick();
  bits[0] = gpioRead(GPIO);

  for (i = 1; i < BLEN; i++) {
    while (gpioTick() < first_tick + i*SAMP_PERIOD_USEC) {}
    bits[i] = gpioRead(GPIO);
  }
}

/* Count errors in bit (or marker) which occuplies 1 second.  Do this by
 * sum of xor with an ideal 0, 1, or marker.  When decoding, call this function
 * for all of 0, 1, and marker.  Decode as the value that showed the fewest
 * errors.  This is the key to how this program works. */

uint32_t xor_sec(uint32_t samp_idx, uint32_t zero_len, uint32_t one_len)
{
  uint32_t i, sum = 0;

  for (i = samp_idx; i < samp_idx + zero_len; i++) {
    sum += 0 ^ bits[i];
  }
  samp_idx += zero_len;
  for (i = samp_idx; i < samp_idx + one_len; i++) {
    sum += 1 ^ bits[i];
  }

  return sum;
}

uint32_t xor_mark(uint32_t samp_idx)
{
  return xor_sec(samp_idx, 800/SAMP_PERIOD, 200/SAMP_PERIOD);
}

uint32_t xor_zero(uint32_t samp_idx)
{
  return xor_sec(samp_idx, 200/SAMP_PERIOD, 800/SAMP_PERIOD);
}

uint32_t xor_one(uint32_t samp_idx)
{
  return xor_sec(samp_idx, 500/SAMP_PERIOD, 500/SAMP_PERIOD);
}


/* frame_const_fields contains the position (second) of each fixed-value field
 * in the frame.  These are either zeros (unused bits) or markers.
 */

struct {
  int32_t type; /* 0 for zero, 2 for mark */
  int32_t sec; /* seconds into frame */
} frame_const_fields[] = {
  {2, 0},
  {0, 4},
  {2, 9},
  {0, 10},
  {0, 11},
  {0, 14},
  {2, 19},
  {0, 20},
  {0, 21},
  {0, 24},
  {2, 29},
  {0, 34},
  {0, 35},
  {2, 39},
  {0, 44},
  {2, 49},
  {0, 54},
  {2, 59}
};

/* Tests how well a given sample works as the start of a frame.  Works by
 * computing the error with respect to all known fields in a frame that
 * have a fixed value. */

uint32_t xor_frame(uint32_t samp_idx, uint32_t min_val)
{
  uint32_t i, sum = 0, res;

  for (i = 0; i < sizeof(frame_const_fields)/sizeof(frame_const_fields[0]); i++) {

    switch (frame_const_fields[i].type) {
    case 0:
      res = xor_zero(samp_idx + frame_const_fields[i].sec*SAMPLES_PER_SEC);
      sum += res;
      break;
    case 2:
      res = xor_mark(samp_idx + frame_const_fields[i].sec*SAMPLES_PER_SEC);
      sum += res;
      break;
    }
    if (sum > min_val) {
      /* no reason to keep going-- better choice of frame start has already been
         found */
      return sum;
    }
  }
  
  return sum;
}

/* Search through the bit buffer for the sample that best works as the start of the
 * frame. This will always find something-- even random data has a sample that works
 * best as a frame, even if it works poorly! Random data would yield a decode with
 * a very poor score (count of sampled bits that are in error. */

uint32_t find_frame(uint32_t *min_val)
{
  uint32_t samp_idx, min_idx, lmin, res;

  min_idx = BLEN + BLEN;
  lmin = SAMPLES_PER_SEC * 120;

  for (samp_idx = 0; samp_idx < BLEN - SAMPLES_PER_SEC*60; samp_idx++) {
    res =  xor_frame(samp_idx, lmin);
    if (res < lmin) {
      lmin = res;
      min_idx = samp_idx;
    }
  }

  *min_val = lmin;
  return min_idx;
}



/* decode_sec
 *
 *  return 0, 1, or 2 for 0, 1, or mark.  Also score (error count) of which matched
 *  best.  A poor score means the decode is likely wrong.
 */

uint32_t decode_sec(uint32_t samp_idx, uint32_t *score)
{
  uint32_t zero_score, one_score, mark_score, best_type, lscore;

  zero_score = xor_zero(samp_idx);
  one_score = xor_one(samp_idx);
  mark_score = xor_mark(samp_idx);

  if (one_score < zero_score) {
    best_type = 1;
    lscore = one_score;
  } else {
    best_type = 0;
    lscore = zero_score;
  }

  if (mark_score < lscore) {
    best_type = 2;
    lscore = mark_score;
  }

  *score = lscore;

  return best_type;
}


/* decode_field
 *
 * decode a complete field such as minutes or hours.  Returns the field value and places
 * the decode quality score in score.
 *
 * If a bit best decodes as a mark, the field decode fails.  Value returned is 0 and the
 * score is DECODE_FAILURE.  worst_score is the number of errors in the bit withih the field
 * that had the most errors.
 */

uint32_t decode_field(uint32_t frame_idx, code_t *code, uint32_t code_len, uint32_t *score,
		      uint32_t *worst_score)
{
  uint32_t i, lscore = 0, res, res_score, field_val = 0;

  *worst_score = 0;

  for (i = 0; i < code_len; i++) {
    res = decode_sec(frame_idx + code[i].bit*SAMPLES_PER_SEC, &res_score);
    if (res_score > *worst_score) *worst_score = res_score;
    if (res == 2) {
      *score = DECODE_FAILURE;
      *worst_score = SAMPLES_PER_SEC;
      return 0;
    } else {
      field_val += code[i].weight*res;
      lscore += res_score;
    }
  }

  *score = lscore;
  return field_val;
}

/* Decode the frame located by seaching for the sample that produced the best
 * match to the unchanging parts of frames */

uint32_t decode_frame(uint32_t frame_idx)
{
  uint32_t i, res, res_score, score = 0, worst_score;

  for (i = 0; i < sizeof(frame)/sizeof(frame[0]); i++) {
    res = decode_field(frame_idx, frame[i].code, frame[i].code_len, &res_score, &worst_score);
    frame[i].score = res_score;
    frame[i].worst_score = worst_score;
    score += res_score;
    frame[i].value = res;
  }

  return score;
}

/* Display the frame with sampled bits organized by seconds within the frame.  If
 * the decode is correct, the first second will be a marker.  If the decode is
 * correct, you can see the values of each second by eye.
 *
 * A marker is 80% zeros followed by 20% ones.
 * A zero is 20% zeros followed by 80% ones.
 * A one is 50% zeros followed by 50% ones.
 *
 * Note some people may use an inverted definition.
 */

void print_frame(uint32_t samp_idx)
{
  uint32_t i, secs, lb_mod;
  
  printf("   Sec Sample          Samples in Second\n");
  printf("   --- ------  ----------------------------------------");

  secs = 0;
  lb_mod = samp_idx % SAMPLES_PER_SEC;
  for (i = samp_idx; i < samp_idx + 60*SAMPLES_PER_SEC; i++) {
    if (i % SAMPLES_PER_SEC == lb_mod) printf("\n   %03u (%04u): ", secs++, i);
    printf("%u", bits[i]);
  }
  printf("\n");
}

void daynum_to_month_day(uint32_t daynum, uint32_t *month, uint32_t *day, uint32_t is_leap_year)
{
  uint32_t daysums[] = {31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365};
  uint32_t i;

  if (is_leap_year) for (i = 1; i < 13; i++) daysums[i] += 1;
  *month = 0;
  while (daysums[*month] < daynum) {
    *month += 1;
  }
  if (*month > 0)
    *day = daynum - daysums[*month - 1];
  else
    *day = daynum;

  *month += 1;
}

int main(int argc, char *argv[])
{
  int opt, print_flag = 0;
  uint32_t i, frame_idx, min_val, score, month, day;
  char *infilename = NULL, *outfilename = NULL;
  uint32_t start, end, total_code_len, frame_worst_sec_score;

  while ((opt = getopt(argc, argv, "i:o:ph")) != -1) {
    switch (opt) {
    case 'i':
      infilename = optarg;
      break;
    case 'o':
      outfilename = optarg;
      break;
    case 'p':
      print_flag = 1;
      break;
    case 'h':
    defualt:
      fprintf(stderr, "Usage: wwvb_dec [-i in_filename] [-o out_filename] [-p]\n");
      fprintf(stderr, "          -i filename  : read samples from file rather than GPIO.\n");
      fprintf(stderr, "          -o filename  : write samples to file.\n");
      fprintf(stderr, "          -p filename  : ASCII print the frame.\n");
      exit(EXIT_FAILURE);
    }
  }


  if (infilename == NULL) {

    gpioCfgClock(5, 1, 1); /* this is defaults anyway */

    if (gpioInitialise()<0) {
      fprintf(stderr, "Could not initialize GPIO library\n");
      return EXIT_FAILURE;
    }
    start = gpioTick();
    fill_buffer_gpio();
    end = gpioTick();
  } else {

    fill_buffer_file(infilename);
    
  }

  frame_idx = find_frame(&min_val);
  printf("\nFound frame at sample %u, score %u, fill time %u usec\n", frame_idx,
	 min_val, end - start);

  if (print_flag) print_frame(frame_idx);

  score = decode_frame(frame_idx);

  daynum_to_month_day(frame[DAYNUM].value, &month, &day, frame[LYI].value);

  printf("  Time: %02u:%02u                  (%u/%.2f-%02u, %u/%.2f-%02u)\n",
	 frame[HOURS].value, frame[MINUTES].value,
	 frame[HOURS].score, frame[HOURS].score/(float)frame[HOURS].code_len, frame[HOURS].worst_score,
	 frame[MINUTES].score, frame[MINUTES].score/(float)frame[MINUTES].code_len, frame[MINUTES].worst_score);

  printf("  Day Number: %03u of year %02u   (%u/%.2f-%02u, %u/%.2f-%02u)\n",
	 frame[DAYNUM].value, frame[YEAR].value,
         frame[DAYNUM].score, frame[DAYNUM].score/(float)frame[DAYNUM].code_len, frame[DAYNUM].worst_score,
	 frame[YEAR].score, frame[YEAR].score/(float)frame[YEAR].code_len, frame[YEAR].worst_score);

  printf("  LYI: %u, LSW: %u, DST: %02u      (%u/%.2f-%02u, %u/%.2f-%02u, %u/%.2f-%02u)\n",
	 frame[LYI].value, frame[LSW].value, frame[DST].value,
	 frame[LYI].score, frame[LYI].score/(float)frame[LYI].code_len, frame[LYI].worst_score,
	 frame[LSW].score, frame[LSW].score/(float)frame[LSW].code_len, frame[LSW].worst_score,
	 frame[DST].score, frame[DST].score/(float)frame[DST].code_len, frame[DST].worst_score);

  total_code_len = 0;
  frame_worst_sec_score = 0;
  for (i = 0; i < sizeof(frame)/sizeof(frame[0]); i++) {
    if (frame[i].worst_score > frame_worst_sec_score) frame_worst_sec_score = frame[i].worst_score;
    total_code_len += frame[i].code_len;
  }
  printf("  Total decode score %u/%.2f-%02u (lower is better)\n\n", score, score/(float)total_code_len,
	 frame_worst_sec_score);
  
  printf("  Summary: %02u:%02u UT1 on %02u/%02u/20%02u - %02u ", frame[HOURS].value, frame[MINUTES].value,
	 month, day, frame[YEAR].value, frame_worst_sec_score);
  if (frame_worst_sec_score < 7)
    printf("LIKELY OK\n");
  else if (frame_worst_sec_score < 10)
    printf("NOT RELIABLE\n");
  else
    printf("PROBABLY BAD\n");

  if (infilename == NULL) gpioTerminate();

  if (outfilename != NULL) save_buffer_file(outfilename);
  
  return EXIT_SUCCESS;
}
