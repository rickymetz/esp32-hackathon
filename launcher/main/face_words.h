/*
 * The words face's clock arithmetic, isolated so it can be unit-tested.
 *
 * It lives in a header as static inline functions rather than in
 * launcher_face.c because the simulator's unit-test binary links neither the
 * launcher nor LVGL's display stack -- and this is pure integer arithmetic
 * with no need of either.
 *
 * It was pulled out after the AM/PM bug: "twenty to twelve" names the NEXT
 * hour, but the label was derived from the raw reading, so the face
 * contradicted itself for the last half of every twelfth hour. A golden
 * screenshot could not defend this -- two letters of text is 0.2% of the
 * frame and sits under the drift threshold -- so the guard has to be a test
 * of the arithmetic itself.
 */
#pragma once

/* Which of the twelve five-minute phrases ("o'clock", "five past", ...
 * "five to") the minute falls in. */
static inline int launcher_face_words_slot(int m)
{
    return ((m + 2) / 5) % 12;
}

/* The hour the spoken phrase NAMES, on a 24-hour clock.
 *
 * From "twenty-five to" onward the phrase refers to the next hour, so this
 * is what both the hour word and the AM/PM label must be derived from. The
 * rollover is tested on the UNWRAPPED value: a `% 12` that collapsed slot 12
 * to 0 first meant the branch never ran at :58 or :59 and the face read a
 * whole hour early. */
static inline int launcher_face_spoken_h24(int h, int m)
{
    return ((m + 2) / 5) >= 7 ? (h + 1) % 24 : h;
}
