
/***************************************************************************
 * osscan.cc -- Routines used for OS detection via TCP/IP fingerprinting.  *
 * For more information on how this works in Nmap, see my paper at         *
 * https://nmap.org/osdetect/                                               *
 *                                                                         *
 ***********************IMPORTANT NMAP LICENSE TERMS************************
 *                                                                         *
 * The Nmap Security Scanner is (C) 1996-2022 Nmap Software LLC ("The Nmap *
 * Project"). Nmap is also a registered trademark of the Nmap Project.     *
 *                                                                         *
 * This program is distributed under the terms of the Nmap Public Source   *
 * License (NPSL). The exact license text applying to a particular Nmap    *
 * release or source code control revision is contained in the LICENSE     *
 * file distributed with that version of Nmap or source code control       *
 * revision. More Nmap copyright/legal information is available from       *
 * https://nmap.org/book/man-legal.html, and further information on the    *
 * NPSL license itself can be found at https://nmap.org/npsl/ . This       *
 * header summarizes some key points from the Nmap license, but is no      *
 * substitute for the actual license text.                                 *
 *                                                                         *
 * Nmap is generally free for end users to download and use themselves,    *
 * including commercial use. It is available from https://nmap.org.        *
 *                                                                         *
 * The Nmap license generally prohibits companies from using and           *
 * redistributing Nmap in commercial products, but we sell a special Nmap  *
 * OEM Edition with a more permissive license and special features for     *
 * this purpose. See https://nmap.org/oem/                                 *
 *                                                                         *
 * If you have received a written Nmap license agreement or contract       *
 * stating terms other than these (such as an Nmap OEM license), you may   *
 * choose to use and redistribute Nmap under those terms instead.          *
 *                                                                         *
 * The official Nmap Windows builds include the Npcap software             *
 * (https://npcap.com) for packet capture and transmission. It is under    *
 * separate license terms which forbid redistribution without special      *
 * permission. So the official Nmap Windows builds may not be              *
 * redistributed without special permission (such as an Nmap OEM           *
 * license).                                                               *
 *                                                                         *
 * Source is provided to this software because we believe users have a     *
 * right to know exactly what a program is going to do before they run it. *
 * This also allows you to audit the software for security holes.          *
 *                                                                         *
 * Source code also allows you to port Nmap to new platforms, fix bugs,    *
 * and add new features.  You are highly encouraged to submit your         *
 * changes as a Github PR or by email to the dev@nmap.org mailing list     *
 * for possible incorporation into the main distribution. Unless you       *
 * specify otherwise, it is understood that you are offering us very       *
 * broad rights to use your submissions as described in the Nmap Public    *
 * Source License Contributor Agreement. This is important because we      *
 * fund the project by selling licenses with various terms, and also       *
 * because the inability to relicense code has caused devastating          *
 * problems for other Free Software projects (such as KDE and NASM).       *
 *                                                                         *
 * The free version of Nmap is distributed in the hope that it will be     *
 * useful, but WITHOUT ANY WARRANTY; without even the implied warranty of  *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. Warranties,        *
 * indemnification and commercial support are all available through the    *
 * Npcap OEM program--see https://nmap.org/oem/                            *
 *                                                                         *
 ***************************************************************************/

/* $Id$ */

#include "osscan.h"
#include "NmapOps.h"
#include "charpool.h"
#include "FingerPrintResults.h"
#include "nmap_error.h"
#include "string_pool.h"

#include <errno.h>
#include <time.h>

#include <algorithm>
#include <list>

extern NmapOps o;

FingerPrintDB::FingerPrintDB() : MatchPoints(NULL) {
}

FingerPrintDB::~FingerPrintDB() {
  std::vector<FingerPrint *>::iterator current;

  if (MatchPoints != NULL) {
    MatchPoints->erase();
    delete MatchPoints;
  }
  for (current = prints.begin(); current != prints.end(); current++) {
    (*current)->erase();
    delete *current;
  }
}

FingerTest::FingerTest(bool allocResults) : name(NULL), results(NULL) {
  if (allocResults)
    this->results = new std::vector<struct AVal>;
}

void FingerTest::erase() {
  if (this->results) {
    delete this->results;
    this->results = NULL;
  }
}

void FingerPrint::sort() {
  unsigned int i;

  for (i = 0; i < tests.size(); i++)
    std::stable_sort(tests[i].results->begin(), tests[i].results->end());
  std::stable_sort(tests.begin(), tests.end());
}

void FingerPrint::erase() {
  for (std::vector<FingerTest>::iterator t = this->tests.begin();
      t != this->tests.end(); t++) {
    t->erase();
  }
}

/* Compare an observed value (e.g. "45") against an OS DB expression (e.g.
   "3B-47" or "8|A" or ">10"). Return true iff there's a match. The syntax uses
     < (less than)
     > (greater than)
     | (or)
     - (range)
   No parentheses are allowed. */
static bool expr_match(const char *val, const char *expr) {
  const char *p, *q, *q1;  /* OHHHH YEEEAAAAAHHHH!#!@#$!% */
  char *endptr;
  unsigned int val_num, expr_num, expr_num1;
  bool is_numeric;

  p = expr;

  val_num = strtol(val, &endptr, 16);
  is_numeric = !*endptr;
  // TODO: this could be a lot faster if we compiled fingerprints to a bytecode
  // instead of re-parsing every time.
  do {
    q = strchr(p, '|');
    if (is_numeric && (*p == '<' || *p == '>')) {
      expr_num = strtol(p + 1, &endptr, 16);
      if (endptr == q || !*endptr) {
        if ((*p == '<' && val_num < expr_num)
            || (*p == '>' && val_num > expr_num)) {
          return true;
        }
      }
    } else if (is_numeric && ((q1 = strchr(p, '-')) != NULL)) {
      expr_num = strtol(p, &endptr, 16);
      if (endptr == q1) {
        expr_num1 = strtol(q1 + 1, &endptr, 16);
        if (endptr == q || !*endptr) {
          assert(expr_num1 > expr_num);
          if (val_num >= expr_num && val_num <= expr_num1) {
            return true;
          }
        }
      }
    } else {
      if ((q && !strncmp(p, val, q - p)) || (!q && !strcmp(p, val))) {
        return true;
      }
    }
    if (q)
      p = q + 1;
  } while (q);

  return false;
}

/* Returns true if perfect match -- if num_subtests &
   num_subtests_succeeded are non_null it ADDS THE NEW VALUES to what
   is already there.  So initialize them to zero first if you only
   want to see the results from this match.  if shortcircuit is zero,
   it does all the tests, otherwise it returns when the first one
   fails.  If you want details of the match process printed, pass n
   onzero for 'verbose'.  If points is non-null, it is examined to
   find the number of points for each test in the fprint AVal and use
   that the increment num_subtests and num_subtests_succeeded
   appropriately.  If it is NULL, each test is worth 1 point.  In that
   case, you may also pass in the group name (SEQ, T1, etc) to have
   that extra info printed.  If you pass 0 for verbose, you might as
   well pass NULL for testGroupName as it won't be used. */
static int AVal_match(const FingerTest *reference, const FingerTest *fprint, const FingerTest *points,
                      unsigned long *num_subtests,
                      unsigned long *num_subtests_succeeded, int shortcut,
                      int verbose) {
  std::vector<struct AVal>::const_iterator current_ref, prev_ref;
  std::vector<struct AVal>::const_iterator current_fp, prev_fp;
  std::vector<struct AVal>::const_iterator current_points;
  int subtests = 0, subtests_succeeded=0;
  int pointsThisTest = 1;
  char *endptr;

  /* We rely on AVals being sorted by attribute. */
  prev_ref = reference->results->end();
  prev_fp = fprint->results->end();
  current_ref = reference->results->begin();
  current_fp = fprint->results->begin();
  current_points = points->results->begin();
  while (current_ref != reference->results->end()
    && current_fp != fprint->results->end()) {
    int d;

    /* Check for sortedness. */
    if (prev_ref != reference->results->end())
      assert(*prev_ref < *current_ref);
    if (prev_fp != fprint->results->end())
      assert(*prev_fp < *current_fp);

    d = strcmp(current_ref->attribute, current_fp->attribute);
    if (d == 0) {
      for (; current_points != points->results->end(); current_points++) {
        if (strcmp(current_ref->attribute, current_points->attribute) == 0)
          break;
      }
      if (current_points == points->results->end())
        fatal("%s: Failed to find point amount for test %s.%s", __func__, reference->name ? reference->name : "", current_ref->attribute);
      errno = 0;
      pointsThisTest = strtol(current_points->value, &endptr, 10);
      if (errno != 0 || *endptr != '\0' || pointsThisTest < 0)
        fatal("%s: Got bogus point amount (%s) for test %s.%s", __func__, current_points->value, reference->name ? reference->name : "", current_ref->attribute);
      subtests += pointsThisTest;

      if (expr_match(current_fp->value, current_ref->value)) {
        subtests_succeeded += pointsThisTest;
      } else {
        if (shortcut) {
          if (num_subtests)
            *num_subtests += subtests;
          return 0;
        }
        if (verbose)
          log_write(LOG_PLAIN, "%s.%s: \"%s\" NOMATCH \"%s\" (%d %s)\n", reference->name,
                    current_ref->attribute, current_fp->value,
                    current_ref->value, pointsThisTest, (pointsThisTest == 1) ? "point" : "points");
      }
    }

    if (d <= 0) {
      prev_ref = current_ref;
      current_ref++;
    }
    if (d >= 0) {
      prev_fp = current_fp;
      current_fp++;
    }
  }
  if (num_subtests)
    *num_subtests += subtests;
  if (num_subtests_succeeded)
    *num_subtests_succeeded += subtests_succeeded;

  return (subtests == subtests_succeeded) ? 1 : 0;
}

/* Compares 2 fingerprints -- a referenceFP (can have expression
   attributes) with an observed fingerprint (no expressions).  If
   verbose is nonzero, differences will be printed.  The comparison
   accuracy (between 0 and 1) is returned).  If MatchPoints is not NULL, it is
   a special "fingerprints" which tells how many points each test is worth. */
double compare_fingerprints(const FingerPrint *referenceFP, const FingerPrint *observedFP,
                            const FingerPrint *MatchPoints, int verbose) {
  std::vector<FingerTest>::const_iterator current_ref, prev_ref;
  std::vector<FingerTest>::const_iterator current_fp, prev_fp;
  std::vector<FingerTest>::const_iterator current_points;
  unsigned long num_subtests = 0, num_subtests_succeeded = 0;
  unsigned long  new_subtests, new_subtests_succeeded;
  assert(referenceFP);
  assert(observedFP);

  /* We rely on tests being sorted by name. */
  prev_ref = referenceFP->tests.end();
  prev_fp = observedFP->tests.end();
  current_ref = referenceFP->tests.begin();
  current_fp = observedFP->tests.begin();
  current_points = MatchPoints->tests.begin();
  while (current_ref != referenceFP->tests.end()
    && current_fp != observedFP->tests.end()) {
    int d;

    /* Check for sortedness. */
    if (prev_ref != referenceFP->tests.end())
      assert(strcmp(prev_ref->name, current_ref->name) < 0);
    if (prev_fp != observedFP->tests.end())
      assert(strcmp(prev_fp->name, current_fp->name) < 0);

    d = strcmp(current_ref->name, current_fp->name);
    if (d == 0) {
      new_subtests = new_subtests_succeeded = 0;
      for (; current_points != MatchPoints->tests.end(); current_points++) {
        if (strcmp(current_ref->name, current_points->name) == 0)
          break;
      }
      if (current_points == MatchPoints->tests.end())
        fatal("%s: Failed to locate test %s in MatchPoints directive of fingerprint file", __func__, current_ref->name);

      AVal_match(&*current_ref, &*current_fp, &*current_points,
                 &new_subtests, &new_subtests_succeeded, 0, verbose);
      num_subtests += new_subtests;
      num_subtests_succeeded += new_subtests_succeeded;
    }

    if (d <= 0) {
      prev_ref = current_ref;
      current_ref++;
    }
    if (d >= 0) {
      prev_fp = current_fp;
      current_fp++;
    }
  }

  assert(num_subtests_succeeded <= num_subtests);
  return (num_subtests) ? (num_subtests_succeeded / (double) num_subtests) : 0;
}

/* Takes a fingerprint and looks for matches inside the passed in
   reference fingerprint DB.  The results are stored in in FPR (which
   must point to an instantiated FingerPrintResultsIPv4 class) -- results
   will be reverse-sorted by accuracy.  No results below
   accuracy_threshold will be included.  The max matches returned is
   the maximum that fits in a FingerPrintResultsIPv4 class.  */
void match_fingerprint(const FingerPrint *FP, FingerPrintResultsIPv4 *FPR,
                       const FingerPrintDB *DB, double accuracy_threshold) {
  double FPR_entrance_requirement = accuracy_threshold; /* accuracy must be
                                                           at least this big
                                                           to be added to the
                                                           list */
  std::vector<FingerPrint *>::const_iterator current_os;
  FingerPrint FP_copy;
  double acc;
  int state;
  int skipfp;
  int max_prints = sizeof(FPR->matches) / sizeof(FPR->matches[0]);
  int idx;
  double tmp_acc=0.0, tmp_acc2; /* These are temp buffers for list swaps */
  FingerMatch *tmp_FP = NULL, *tmp_FP2;

  assert(FP);
  assert(FPR);
  assert(accuracy_threshold >= 0 && accuracy_threshold <= 1);

  FP_copy = *FP;
  FP_copy.sort();

  FPR->overall_results = OSSCAN_SUCCESS;

  for (current_os = DB->prints.begin(); current_os != DB->prints.end(); current_os++) {
    skipfp = 0;

    acc = compare_fingerprints(*current_os, &FP_copy, DB->MatchPoints, 0);

    /*    error("Comp to %s: %li/%li=%f", o.reference_FPs1[i]->OS_name, num_subtests_succeeded, num_subtests, acc); */
    if (acc >= FPR_entrance_requirement || acc == 1.0) {

      state = 0;
      for (idx=0; idx < FPR->num_matches; idx++) {
        if (strcmp(FPR->matches[idx]->OS_name, (*current_os)->match.OS_name) == 0) {
          if (FPR->accuracy[idx] >= acc) {
            skipfp = 1; /* Skip it -- a higher version is already in list */
          } else {
            /* We must shift the list left to delete this sucker */
            memmove(FPR->matches + idx, FPR->matches + idx + 1,
                    (FPR->num_matches - 1 - idx) * sizeof(FingerPrint *));
            memmove(FPR->accuracy + idx, FPR->accuracy + idx + 1,
                    (FPR->num_matches - 1 - idx) * sizeof(double));
            FPR->num_matches--;
            FPR->accuracy[FPR->num_matches] = 0;
          }
          break; /* There can only be 1 in the list with same name */
        }
      }

      if (!skipfp) {
        /* First we check whether we have overflowed with perfect matches */
        if (acc == 1) {
          /*      error("DEBUG: Perfect match #%d/%d", FPR->num_perfect_matches + 1, max_prints); */
          if (FPR->num_perfect_matches == max_prints) {
            FPR->overall_results = OSSCAN_TOOMANYMATCHES;
            return;
          }
          FPR->num_perfect_matches++;
        }

        /* Now we add the sucker to the list */
        state = 0; /* Have not yet done the insertion */
        for (idx=-1; idx < max_prints -1; idx++) {
          if (state == 1) {
            /* Push tmp_acc and tmp_FP onto the next idx */
            tmp_acc2 = FPR->accuracy[idx+1];
            tmp_FP2 = FPR->matches[idx+1];

            FPR->accuracy[idx+1] = tmp_acc;
            FPR->matches[idx+1] = tmp_FP;

            tmp_acc = tmp_acc2;
            tmp_FP = tmp_FP2;
          } else if (FPR->accuracy[idx + 1] < acc) {
            /* OK, I insert the sucker into the next slot ... */
            tmp_acc = FPR->accuracy[idx+1];
            tmp_FP = FPR->matches[idx+1];
            FPR->matches[idx+1] = &(*current_os)->match;
            FPR->accuracy[idx+1] = acc;
            state = 1;
          }
        }
        if (state != 1) {
          fatal("Bogus list insertion state (%d) -- num_matches = %d num_perfect_matches=%d entrance_requirement=%f", state, FPR->num_matches, FPR->num_perfect_matches, FPR_entrance_requirement);
        }
        FPR->num_matches++;
        /* If we are over max_prints, one was shoved off list */
        if (FPR->num_matches > max_prints)
          FPR->num_matches = max_prints;

        /* Calculate the new min req. */
        if (FPR->num_matches == max_prints) {
          FPR_entrance_requirement = FPR->accuracy[max_prints - 1] + 0.00001;
        }
      }
    }
  }

  if (FPR->num_matches == 0 && FPR->overall_results == OSSCAN_SUCCESS)
    FPR->overall_results = OSSCAN_NOMATCHES;

  return;
}

static const char *dist_method_fp_string(enum dist_calc_method method)
{
  const char *s = "";

  switch (method) {
  case DIST_METHOD_NONE:
    s = "";
    break;
  case DIST_METHOD_LOCALHOST:
    s = "L";
    break;
  case DIST_METHOD_DIRECT:
    s = "D";
    break;
  case DIST_METHOD_ICMP:
    s = "I";
    break;
  case DIST_METHOD_TRACEROUTE:
    s = "T";
    break;
  }

  return s;
}

/* Writes an informational "Test" result suitable for including at the
   top of a fingerprint.  Gives info which might be useful when the
   FPrint is submitted (eg Nmap version, etc).  Result is written (up
   to ostrlen) to the ostr var passed in */
void WriteSInfo(char *ostr, int ostrlen, bool isGoodFP,
                                const char *engine_id,
                                const struct sockaddr_storage *addr, int distance,
                                enum dist_calc_method distance_calculation_method,
                                const u8 *mac, int openTcpPort,
                                int closedTcpPort, int closedUdpPort) {
  struct tm ltime;
  int err;
  time_t timep;
  char dsbuf[10], otbuf[8], ctbuf[8], cubuf[8], dcbuf[8];
  char macbuf[16];
  timep = time(NULL);
  err = n_localtime(&timep, &ltime);
  if (err)
    error("Error in localtime: %s", strerror(err));

  otbuf[0] = '\0';
  if (openTcpPort != -1)
    Snprintf(otbuf, sizeof(otbuf), "%d", openTcpPort);
  ctbuf[0] = '\0';
  if (closedTcpPort != -1)
    Snprintf(ctbuf, sizeof(ctbuf), "%d", closedTcpPort);
  cubuf[0] = '\0';
  if (closedUdpPort != -1)
    Snprintf(cubuf, sizeof(cubuf), "%d", closedUdpPort);

  dsbuf[0] = '\0';
  if (distance != -1)
    Snprintf(dsbuf, sizeof(dsbuf), "%%DS=%d", distance);
  if (distance_calculation_method != DIST_METHOD_NONE)
    Snprintf(dcbuf, sizeof(dcbuf), "%%DC=%s", dist_method_fp_string(distance_calculation_method));
  else
    dcbuf[0] = '\0';

  macbuf[0] = '\0';
  if (mac)
    Snprintf(macbuf, sizeof(macbuf), "%%M=%02X%02X%02X", mac[0], mac[1], mac[2]);

  Snprintf(ostr, ostrlen, "SCAN(V=%s%%E=%s%%D=%d/%d%%OT=%s%%CT=%s%%CU=%s%%PV=%c%s%s%%G=%c%s%%TM=%X%%P=%s)",
                   NMAP_VERSION, engine_id, err ? 0 : ltime.tm_mon + 1, err ? 0 : ltime.tm_mday,
                   otbuf, ctbuf, cubuf, isipprivate(addr) ? 'Y' : 'N', dsbuf, dcbuf, isGoodFP ? 'Y' : 'N',
                   macbuf, (int) timep, NMAP_PLATFORM);
}

/* Puts a textual representation of the test in s.
   No more than n bytes will be written. Unless n is 0, the string is always
   null-terminated. Returns the number of bytes written, excluding the
   terminator. */
static int test2str(const FingerTest *test, char *s, const size_t n) {
  std::vector<struct AVal>::const_iterator av;
  char *p;
  char *end;
  size_t len;

  if (n == 0)
    return 0;

  p = s;
  end = s + n - 1;

  len = strlen(test->name);
  if (p + len > end)
    goto error;

  memcpy(p, test->name, len);
  p += len;
  if (p + 1 > end)
    goto error;
  *p++ = '(';

  for (av = test->results->begin(); av != test->results->end(); av++) {
    if (av != test->results->begin()) {
      if (p + 1 > end)
        goto error;
      *p++ = '%';
    }
    len = strlen(av->attribute);
    if (p + len > end)
      goto error;
    memcpy(p, av->attribute, len);
    p += len;
    if (p + 1 > end)
      goto error;
    *p++ = '=';
    len = strlen(av->value);
    if (p + len > end)
      goto error;
    memcpy(p, av->value, len);
    p += len;
  }

  if (p + 1 > end)
    goto error;
  *p++ = ')';

  *p = '\0';

  return p - s;

error:
  *s = '\0';

  return -1;
}

// Like strchr, but don't go past end. Nulls not handled specially.
static const char *strchr_p(const char *str, const char *end, char c) {
  assert(str && end >= str);
  for (const char *q = str; q < end; q++) {
    if (*q == c)
      return q;
  }
  return NULL;
}

static std::vector<struct AVal> *str2AVal(const char *str, const char *end) {
  int i = 1;
  int count = 1;
  const char *q = str, *p=str;
  std::vector<struct AVal> *AVs = new std::vector<struct AVal>;

  if (!*str || str == end)
    return AVs;

  /* count the AVals */
  while ((q = strchr_p(q, end, '%'))) {
    count++;
    q++;
  }

  AVs->reserve(count);
  for (i = 0; i < count; i++) {
    struct AVal av;

    q = strchr_p(p, end, '=');
    if (!q) {
      fatal("Parse error with AVal string (%s) in nmap-os-db file", str);
    }
    av.attribute = string_pool_substr(p, q);
    p = q+1;
    if (i < count - 1) {
      q = strchr_p(p, end, '%');
      if (!q) {
        fatal("Parse error with AVal string (%s) in nmap-os-db file", str);
      }
      av.value = string_pool_substr(p, q);
    } else {
      av.value = string_pool_substr(p, end);
    }
    p = q + 1;
    AVs->push_back(av);
  }

  return AVs;
}

/* Compare two AVal chains literally, without evaluating the value of either one
   as an expression. This is used by mergeFPs. Unlike with AVal_match, it is
   always the case that test_match_literal(a, b) == test_match_literal(b, a). */
static bool test_match_literal(const FingerTest *a, const FingerTest *b) {
  std::vector<struct AVal>::const_iterator ia, ib;

  for (ia = a->results->begin(), ib = b->results->begin();
    ia != a->results->end() && ib != b->results->end();
    ia++, ib++) {
    if (strcmp(ia->attribute, ib->attribute) != 0)
      return false;
  }
  if (ia != a->results->end() || ib != b->results->end())
    return false;

  return true;
}

/* This is a less-than relation predicate that establishes the preferred order
   of tests when they are displayed. Returns true if and only if the test a
   should come before the test b. */
static bool FingerTest_lessthan(const FingerTest* a, const FingerTest* b) {
  /* This defines the order in which test lines should appear. */
  const char *TEST_ORDER[] = {
    "SEQ", "OPS", "WIN", "ECN",
    "T1", "T2", "T3", "T4", "T5", "T6", "T7",
    "U1", "IE"
  };
  unsigned int i;
  int ia, ib;

  /* The indices at which the test names were found in the list. -1 means "not
     found." */
  ia = -1;
  ib = -1;
  /* Look up the test names in the list. */
  for (i = 0; i < sizeof(TEST_ORDER) / sizeof(*TEST_ORDER); i++) {
    if (ia == -1 && strcmp(a->name, TEST_ORDER[i]) == 0)
      ia = i;
    if (ib == -1 && strcmp(b->name, TEST_ORDER[i]) == 0)
      ib = i;
    /* Once we've found both tests we can stop searching. */
    if (ia != -1 && ib != -1)
      break;
  }
  /* If a test name was not found, it probably indicates an error in another
     part of the code. */
  if (ia == -1)
    fatal("%s received an unknown test name \"%s\".\n", __func__, a->name);
  if (ib == -1)
    fatal("%s received an unknown test name \"%s\".\n", __func__, b->name);

  return ia < ib;
}

/* Merges the tests from several fingerprints into a character string
   representation. Tests that are identical between more than one fingerprint
   are included only once. If wrapit is true, the string is wrapped for
   submission. */
const char *mergeFPs(FingerPrint *FPs[], int numFPs, bool isGoodFP,
                           const struct sockaddr_storage *addr, int distance,
                           enum dist_calc_method distance_calculation_method,
                           const u8 *mac, int openTcpPort, int closedTcpPort,
                           int closedUdpPort, bool wrapit) {
  static char str[10240];
  static char wrapstr[10240];

  char *p;
  int i;
  char *end = str + sizeof(str) - 1; /* Last byte allowed to write into */
  std::list<const FingerTest *> tests;
  std::list<const FingerTest *>::iterator iter;
  std::vector<FingerTest>::iterator ft;

  if (numFPs <= 0)
    return "(None)";
  else if (numFPs > 32)
    return "(Too many)";

  /* Copy the tests from each fingerprint into a flat list. */
  for (i = 0; i < numFPs; i++) {
    for (ft = FPs[i]->tests.begin(); ft != FPs[i]->tests.end(); ft++)
      tests.push_back(&*ft);
  }

  /* Put the tests in the proper order and ensure that tests with identical
     names are contiguous. */
  tests.sort(FingerTest_lessthan);

  /* Delete duplicate tests to ensure that all the tests are unique. One test is
     a duplicate of the other if it has the same name as the first and the two
     results lists match. */
  for (iter = tests.begin(); iter != tests.end(); iter++) {
    std::list<const FingerTest *>::iterator tmp_i, next;
    tmp_i = iter;
    tmp_i++;
    while (tmp_i != tests.end() && strcmp((*iter)->name, (*tmp_i)->name) == 0) {
      next = tmp_i;
      next++;
      if (test_match_literal(*iter, *tmp_i)) {
        /* This is a duplicate test. Remove it. */
        tests.erase(tmp_i);
      }
      tmp_i = next;
    }
  }

  /* A safety check to make sure that no tests were lost in merging. */
  for (i = 0; i < numFPs; i++) {
    for (ft = FPs[i]->tests.begin(); ft != FPs[i]->tests.end(); ft++) {
      for (iter = tests.begin(); iter != tests.end(); iter++) {
        if (strcmp((*iter)->name, ft->name) == 0 && test_match_literal(*iter, &*ft)) {
            break;
        }
      }
      if (iter == tests.end()) {
        char buf[200];
        test2str(&*ft, buf, sizeof(buf));
        fatal("The test %s was somehow lost in %s.\n", buf, __func__);
      }
    }
  }

  memset(str, 0, sizeof(str));

  p = str;

  /* Lets start by writing the fake "SCAN" test for submitting fingerprints */
  WriteSInfo(p, sizeof(str), isGoodFP, "4", addr, distance, distance_calculation_method, mac, openTcpPort, closedTcpPort, closedUdpPort);
  p = p + strlen(str);
  if (!wrapit)
    *p++ = '\n';

  assert(p <= end);

  /* Append the string representation of each test to the result string. */
  for (iter = tests.begin(); iter != tests.end(); iter++) {
    int len;

    len = test2str(*iter, p, end - p + 1);
    if (len == -1)
      break;
    p += len;
    if (!wrapit) {
      if (p + 1 > end)
        break;
      *p++ = '\n';
    }
  }

  /* If we bailed out of the loop early it was because we ran out of space. */
  if (iter != tests.end())
    fatal("Merged fingerprint too long in %s.\n", __func__);

  *p = '\0';

  if (!wrapit) {
    return str;
  } else {
    /* Wrap the str. */
    int len;
    char *p1 = wrapstr;
    end = wrapstr + sizeof(wrapstr) - 1;

    p = str;

    while (*p && end-p1 >= 3) {
      len = 0;
      strcpy(p1, "OS:"); p1 += 3; len +=3;
      while (*p && len <= FP_RESULT_WRAP_LINE_LEN && end-p1 > 0) {
        *p1++ = *p++;
        len++;
      }
      if (end-p1 <= 0) {
        fatal("Wrapped result too long!\n");
        break;
      }
      *p1++ = '\n';
    }
    *p1 = '\0';

    return wrapstr;
  }
}

const char *fp2ascii(const FingerPrint *FP) {
  static char str[2048];
  std::vector<FingerTest>::const_iterator iter;
  char *p = str;

  if (!FP)
    return "(None)";

  for (iter = FP->tests.begin(); iter != FP->tests.end(); iter++) {
    int len;

    len = test2str(&*iter, p, sizeof(str) - (p - str));
    if (len == -1)
      break;
    p += len;
    if (p + 1 > str + sizeof(str))
      break;
    *p++ = '\n';
  }

  *p = '\0';

  return str;
}

/* Parse a 'Class' line found in the fingerprint file into the current
   FP.  Classno is the number of 'class' lines found so far in the
   current fingerprint.  The function quits if there is a parse error */
static void parse_classline(FingerPrint *FP, const char *thisline, const char *lineend, int lineno) {
  const char *begin, *end;
  struct OS_Classification os_class;

  if (!thisline || lineend - thisline < 6 || strncmp(thisline, "Class ", 6) != 0)
    fatal("Bogus line #%d (%.*s) passed to %s()", lineno, (int)(lineend - thisline), thisline, __func__);

  /* First let's get the vendor name. */
  begin = thisline + 6;
  end = strchr_p(begin, lineend, '|');
  if (end == NULL)
    fatal("Parse error on line %d of fingerprint: %s\n", lineno, thisline);
  os_class.OS_Vendor = string_pool_substr_strip(begin, end);

  /* Next comes the OS family. */
  begin = end + 1;
  end = strchr_p(begin, lineend, '|');
  if (end == NULL)
    fatal("Parse error on line %d of fingerprint: %s\n", lineno, thisline);
  os_class.OS_Family = string_pool_substr_strip(begin, end);

  /* And now the the OS generation. */
  begin = end + 1;
  end = strchr_p(begin, lineend, '|');
  if (end == NULL)
    fatal("Parse error on line %d of fingerprint: %s\n", lineno, thisline);
  /* OS generation is handled specially: instead of an empty string it's
     supposed to be NULL. */
  while (isspace((int) (unsigned char) *begin))
    begin++;
  if (begin < end)
    os_class.OS_Generation = string_pool_substr_strip(begin, end);
  else
    os_class.OS_Generation = NULL;

  /* And finally the device type. */
  begin = end + 1;
  os_class.Device_Type = string_pool_substr_strip(begin, lineend);

  FP->match.OS_class.push_back(os_class);
}

static void parse_cpeline(FingerPrint *FP, const char *thisline, const char *lineend, int lineno) {
  const char *cpe;

  if (FP->match.OS_class.empty())
    fatal("\"CPE\" line without preceding \"Class\" at line %d", lineno);

  OS_Classification& osc = FP->match.OS_class.back();

  if (thisline == NULL || lineend - thisline < 4 || strncmp(thisline, "CPE ", 4) != 0)
    fatal("Bogus line #%d (%.*s) passed to %s()", lineno, (int)(lineend - thisline), thisline, __func__);

  /* The cpe part may be followed by whitespace-separated flags (like "auto"),
     which we ignore. */
  cpe = string_pool_strip_word(thisline + 4, lineend);
  assert(cpe != NULL);
  osc.cpe.push_back(cpe);
}

/* Parses a single fingerprint from the memory region given.  If a
   non-null fingerprint is returned, the user is in charge of freeing it
   when done.  This function does not require the fingerprint to be 100%
   complete since it is used by scripts such as scripts/fingerwatch for
   which some partial fingerpritns are OK. */
/* This function is not currently used by Nmap, but it is present here because
   it is used by fingerprint utilities that link with Nmap object files. */
FingerPrint *parse_single_fingerprint(const char *fprint) {
  int lineno = 0;
  const char *p, *q;
  const char *thisline, *nextline;
  const char * const end = strchr(fprint, '\0');
  FingerPrint *FP;

  FP = new FingerPrint;

  thisline = fprint;

  do /* 1 line at a time */ {
    nextline = strchr_p(thisline, end, '\n');
    if (!nextline)
      nextline = end;
    /* printf("Preparing to handle next line: %s\n", thisline); */

    while (thisline < nextline && isspace((int) (unsigned char) *thisline))
      thisline++;
    if (thisline >= nextline) {
      fatal("Parse error on line %d of fingerprint\n", lineno);
    }

    if (strncmp(thisline, "Fingerprint ", 12) == 0) {
      /* Ignore a second Fingerprint line if it appears. */
      if (FP->match.OS_name == NULL) {
        p = thisline + 12;
        while (p < nextline && isspace((int) (unsigned char) *p))
          p++;

        q = nextline ? nextline : end;
        while (q > p && isspace((int) (unsigned char) *q))
          q--;

        FP->match.OS_name = cp_strndup(p, q - p);
      }
    } else if (strncmp(thisline, "MatchPoints", 11) == 0) {
      p = thisline + 11;
      while (p < nextline && isspace((int) (unsigned char) *p))
        p++;
      if (p != nextline)
        fatal("Parse error on line %d of fingerprint: %.*s\n", lineno, (int)(nextline - thisline), thisline);
    } else if (strncmp(thisline, "Class ", 6) == 0) {

      parse_classline(FP, thisline, nextline, lineno);

    } else if (strncmp(thisline, "CPE ", 4) == 0) {

      parse_cpeline(FP, thisline, nextline, lineno);

    } else if ((q = strchr_p(thisline, nextline, '('))) {
      FingerTest test;
      test.name = string_pool_substr(thisline, q);
      p = q+1;
      q = strchr_p(p, nextline, ')');
      if (!q) {
        fatal("Parse error on line %d of fingerprint: %.*s\n", lineno, (int)(nextline - thisline), thisline);
      }
      test.results = str2AVal(p, q);
      FP->tests.push_back(test);
    } else {
      fatal("Parse error on line %d of fingerprint: %.*s\n", lineno, (int)(nextline - thisline), thisline);
    }

    thisline = nextline; /* Time to handle the next line, if there is one */
    lineno++;
  } while (thisline && thisline < end);

  return FP;
}


FingerPrintDB *parse_fingerprint_file(const char *fname) {
  FingerPrintDB *DB = NULL;
  FingerPrint *current;
  FILE *fp;
  char line[2048];
  int lineno = 0;
  bool parsingMatchPoints = false;

  DB = new FingerPrintDB;

  const char *p, *q; /* OH YEAH!!!! */

  fp = fopen(fname, "r");
  if (!fp)
    pfatal("Unable to open Nmap fingerprint file: %s", fname);

top:
  while (fgets(line, sizeof(line), fp)) {
    lineno++;
    /* Read in a record */
    if (*line == '\n' || *line == '#')
      continue;

fparse:
    if (strncmp(line, "Fingerprint", 11) == 0) {
      parsingMatchPoints = false;
    } else if (strncmp(line, "MatchPoints", 11) == 0) {
      if (DB->MatchPoints)
        fatal("Found MatchPoints directive on line %d of %s even though it has previously been seen in the file", lineno, fname);
      parsingMatchPoints = true;
    } else {
      error("Parse error on line %d of nmap-os-db file: %s", lineno, line);
      continue;
    }

    current = new FingerPrint;

    if (parsingMatchPoints) {
      current->match.OS_name = NULL;
      DB->MatchPoints = current;
    } else {
      DB->prints.push_back(current);
      p = line + 12;
      while (*p && isspace((int) (unsigned char) *p))
        p++;

      q = strpbrk(p, "\n#");
      if (!q)
        fatal("Parse error on line %d of fingerprint: %s", lineno, line);

      while (isspace((int) (unsigned char) *(--q)))
        ;

      if (q < p)
        fatal("Parse error on line %d of fingerprint: %s", lineno, line);

      current->match.OS_name = cp_strndup(p, q - p + 1);
    }

    current->match.line = lineno;

    /* Now we read the fingerprint itself */
    while (fgets(line, sizeof(line), fp)) {
      lineno++;
      if (*line == '#')
        continue;
      if (*line == '\n')
        break;

      q = strchr(line, '\n');

      if (!strncmp(line, "Fingerprint ",12)) {
        goto fparse;
      } else if (strncmp(line, "Class ", 6) == 0) {
        parse_classline(current, line, q, lineno);
      } else if (strncmp(line, "CPE ", 4) == 0) {
        parse_cpeline(current, line, q, lineno);
      } else {
        FingerTest test;
        p = line;
        q = strchr(line, '(');
        if (!q) {
          error("Parse error on line %d of nmap-os-db file: %s", lineno, line);
          goto top;
        }
        test.name = string_pool_substr(p, q);
        p = q+1;
        q = strchr(p, ')');
        if (!q) {
          error("Parse error on line %d of nmap-os-db file: %s", lineno, line);
          goto top;
        }
        test.results = str2AVal(p, q);
        current->tests.push_back(test);
      }
    }
    /* This sorting is important for later comparison of FingerPrints and
       FingerTests. */
    current->sort();
  }

  fclose(fp);
  return DB;
}

FingerPrintDB *parse_fingerprint_reference_file(const char *dbname) {
  char filename[256];

  if (nmap_fetchfile(filename, sizeof(filename), dbname) != 1) {
    fatal("OS scan requested but I cannot find %s file.", dbname);
  }
  /* Record where this data file was found. */
  o.loaded_data_files[dbname] = filename;

  return parse_fingerprint_file(filename);
}
