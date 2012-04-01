#include "jsonsl.h"
#include <assert.h>
#include <limits.h>
#include <ctype.h>

/**
 * This table (predeclared) contains characters which are recognized
 * non-string values.
 */
static jsonsl_special_t *Special_table;
#define extract_special(c) \
    Special_table[(unsigned int)(c & 0xff)]

/**
 * This table (predeclared) contains the tokens and other characters
 * which signal the termination of the non-string values.
 */
static int *Special_Endings;
#define is_special_end(c) \
    Special_Endings[(unsigned int)c & 0xff]

/**
 * This table contains entries for the allowed whitespace
 * as per RFC 4627
 */
static int *Allowed_Whitespace;
#define is_allowed_whitespace(c) \
    Allowed_Whitespace[(unsigned int)c & 0xff]


JSONSL_API
jsonsl_t jsonsl_new(int nlevels)
{
    struct jsonsl_st *jsn =
            calloc(1, sizeof (*jsn) +
                    ( (nlevels-1) * sizeof (struct jsonsl_state_st) )
            );

    jsn->levels_max = nlevels;
    jsn->max_callback_level = -1;
    jsonsl_reset(jsn);
    return jsn;
}

JSONSL_API
void jsonsl_reset(jsonsl_t jsn)
{
    int ii;
    jsn->tok_last = 0;
    jsn->can_insert = 1;
    jsn->pos = 0;
    jsn->level = 0;
    jsn->in_escape = 0;
    jsn->expecting = 0;

    for (ii = 0; ii < jsn->levels_max; ii++) {
        memset(jsn->stack + ii, 0, sizeof(struct jsonsl_state_st));
        jsn->stack[ii].level = ii;
    }
}

JSONSL_API
void jsonsl_destroy(jsonsl_t jsn)
{
    if (jsn) {
        free(jsn);
    }
}

JSONSL_API
void
jsonsl_feed(jsonsl_t jsn, const jsonsl_char_t *bytes, size_t nbytes)
{

#define INVOKE_ERROR(eb) \
    if (jsn->error_callback(jsn, JSONSL_ERROR_##eb, state, (char*)c)) { \
        goto GT_AGAIN; \
    } \
    return;

#define STACK_PUSH \
    if (jsn->level >= (levels_max-1)) { \
        jsn->error_callback(jsn, JSONSL_ERROR_LEVELS_EXCEEDED, state, (char*)c); \
        return; \
    } \
    state = jsn->stack + (++jsn->level); \
    state->ignore_callback = jsn->stack[jsn->level-1].ignore_callback; \
    state->pos_begin = jsn->pos;

#define STACK_POP_NOPOS \
    state->pos_cur = jsn->pos; \
    state = jsn->stack + (--jsn->level);


#define STACK_POP \
    STACK_POP_NOPOS; \
    state->pos_cur = jsn->pos;

#define CALLBACK_AND_POP_NOPOS(T) \
        state->pos_cur = jsn->pos; \
        CALLBACK(T, POP); \
        state = jsn->stack + (--jsn->level);

#define CALLBACK_AND_POP(T) \
        CALLBACK_AND_POP_NOPOS(T); \
        state->pos_cur = jsn->pos;

#define SPECIAL_MAYBE_POP \
    if (state->type == JSONSL_T_SPECIAL) { \
        CALLBACK_AND_POP(SPECIAL); \
        jsn->expecting = 0; \
    }


#define CALLBACK(T, action) \
    if (jsn->call_##T && \
            jsn->max_callback_level > state->level && \
            state->ignore_callback == 0) { \
        \
        if (jsn->action_callback_##action) { \
            jsn->action_callback_##action(jsn, JSONSL_ACTION_##action, state, (jsonsl_char_t*)c); \
        } else if (jsn->action_callback) { \
            jsn->action_callback(jsn, JSONSL_ACTION_##action, state, (jsonsl_char_t*)c); \
        } \
    }

    /**
     * Verifies that we are able to insert the (non-string) item into a hash.
     */
#define ENSURE_HVAL \
    if (state->nelem % 2 == 0 && state->type == JSONSL_T_OBJECT) { \
        INVOKE_ERROR(HKEY_EXPECTED); \
    }

    const jsonsl_uchar_t *c = (jsonsl_uchar_t*)bytes;
    size_t levels_max = jsn->levels_max;
    struct jsonsl_state_st *state = jsn->stack + jsn->level;
    jsn->base = bytes;

    for (; nbytes; nbytes--, jsn->pos++, c++) {
        /* Special escape handling for some stuff */
        if (jsn->in_escape) {
            jsn->in_escape = 0;
            if (*c == 'u') {
                CALLBACK(UESCAPE, UESCAPE);
                if (jsn->return_UESCAPE) {
                    return;
                }
            }
            goto GT_NEXT;
        }

        GT_AGAIN:
        /**
         * Several fast-tracks for common cases:
         */
        if (state->type & JSONSL_Tf_STRINGY) {
            /**
             * For a string we don't care about anything above 0x23 (0x22 == '"')
             * and the backslash (0x5c)
             */
            if ( (*c >= 0x23 && *c != '\\') || (*c == 0x20) ) {
                goto GT_NEXT;
            } else if (*c == '"') {
                /* terminator */
                goto GT_QUOTE;
            } else if (*c < 0x20) {
                /* unescaped whitespace */
                INVOKE_ERROR(WEIRD_WHITESPACE);
            }
        } else if (state->type == JSONSL_T_SPECIAL) {
            if ( (*c >= 0x30 && *c < 0x40) || is_special_end(*c) == 0) {
                /* Most common case. Inside a number */
                goto GT_NEXT;
            } else if (is_allowed_whitespace(*c)) {
                /**
                 * Note how we only check for the whitespace-subset of
                 * special terminators. This because a 'special terminator'
                 * can also have dual-purpose (except for whitespace, which
                 * serves no other function).
                 */
                SPECIAL_MAYBE_POP;
                goto GT_NEXT;
            }
        } else if (is_allowed_whitespace(*c)) {
            /* So we're not special. Harmless insignificant whitespace
             * passthrough
             */
            goto GT_NEXT;
        }

        if (*c == '"') {
            GT_QUOTE:
            jsn->can_insert = 0;
            switch (state->type) {

            /* the end of a string or hash key */
            case JSONSL_T_STRING:
                CALLBACK_AND_POP(STRING);
                goto GT_NEXT;
            case JSONSL_T_HKEY:
                CALLBACK_AND_POP(HKEY);
                goto GT_NEXT;

            case JSONSL_T_OBJECT:
                state->nelem++;
                if ( (state->nelem-1) % 2 ) {
                    /* Odd, this must be a hash value */
                    if (jsn->tok_last != ':') {
                        INVOKE_ERROR(MISSING_TOKEN);
                    }
                    jsn->expecting = ','; /* Can't figure out what to expect next */

                    STACK_PUSH;
                    state->type = JSONSL_T_STRING;
                    CALLBACK(STRING, PUSH);

                } else {
                    /* hash key */
                    if (jsn->expecting != '"') {
                        INVOKE_ERROR(STRAY_TOKEN);
                    }
                    jsn->tok_last = 0;
                    jsn->expecting = ':';

                    STACK_PUSH;
                    state->type = JSONSL_T_HKEY;
                    CALLBACK(HKEY, PUSH);
                }
                goto GT_NEXT;

            case JSONSL_T_LIST:
                state->nelem++;
                STACK_PUSH;
                state->type = JSONSL_T_STRING;
                jsn->expecting = ',';
                CALLBACK(STRING, PUSH);
                goto GT_NEXT;

            case JSONSL_T_SPECIAL:
                INVOKE_ERROR(STRAY_TOKEN);
                break;

            default:
                INVOKE_ERROR(STRING_OUTSIDE_CONTAINER);
                break;
            } /* switch(state->type) */
        } else if (*c == '\\') {
        /* Escape */
            if (! (state->type == JSONSL_T_STRING || state->type == JSONSL_T_HKEY) ) {
                INVOKE_ERROR(ESCAPE_OUTSIDE_STRING);
            }
            jsn->in_escape = 1;
            goto GT_NEXT;
        } /* " or \ */


        /* ignore string content */
        if (state->type & JSONSL_Tf_STRINGY) {
            goto GT_NEXT;
        }

        switch (*c) {
        case ':':
        case ',':

            if (*c == ',') {
                SPECIAL_MAYBE_POP;
                jsn->expecting = ','; /* hack */
            }

            if (jsn->expecting != *c) {
                INVOKE_ERROR(STRAY_TOKEN);
            }

            if (state->type == JSONSL_T_OBJECT && *c == ',') {
                /* end of hash value, expect a string as a hash key */
                jsn->expecting = '"';
            }

            jsn->tok_last = *c;
            jsn->can_insert = 1;
            goto GT_NEXT;

            /* new list or object */
        case '[':
        case '{':
            if (!jsn->can_insert) {
                INVOKE_ERROR(CANT_INSERT);
            }

            ENSURE_HVAL;
            state->nelem++;

            STACK_PUSH;
            /* because the constants match the opening delimiters, we can do this: */
            state->type = *c;
            state->nelem = 0;
            jsn->can_insert = 1;
            if (*c == '{') {
                /* If we're a hash, we expect a key first, which is quouted */
                jsn->expecting = '"';
            }
            if (*c == JSONSL_T_OBJECT) {
                CALLBACK(OBJECT, PUSH);
            } else {
                CALLBACK(LIST, PUSH);
            }
            jsn->tok_last = 0;
            goto GT_NEXT;

            /* closing of list or object */
        case '}':
        case ']':
            SPECIAL_MAYBE_POP;
            jsn->can_insert = 0;
            jsn->level--;
            jsn->expecting = ',';
            if (*c == ']') {
                if (state->type != '[') {
                    INVOKE_ERROR(BRACKET_MISMATCH);
                }
                CALLBACK(LIST, POP);
            } else {
                if (state->type != '{') {
                    INVOKE_ERROR(BRACKET_MISMATCH);
                }
                CALLBACK(OBJECT, POP);
            }
            state = jsn->stack + jsn->level;
            state->pos_cur = jsn->pos;
            goto GT_NEXT;

        default:
            /**
             * Not a string, not a structural token, and not benign whitespace.
             * Technically we should iterate over the character always, but since
             * we are not doing full numerical/value decoding anyway (but only hinting),
             * we only check upon entry.
             */
            if (state->type != JSONSL_T_SPECIAL) {
                int special_flags = extract_special(*c);
                if (!special_flags) {
                    /**
                     * Try to do some heuristics here anyway to figure out what kind of
                     * error this is. The 'special' case is a fallback scenario anyway.
                     */
                    if (*c == '\0') {
                        INVOKE_ERROR(FOUND_NULL_BYTE);
                    } else if (*c < 0x20) {
                        INVOKE_ERROR(WEIRD_WHITESPACE);
                    } else {
                        INVOKE_ERROR(SPECIAL_EXPECTED);
                    }
                }
                ENSURE_HVAL;
                state->nelem++;
                STACK_PUSH;
                state->type = JSONSL_T_SPECIAL;
                state->special_flags = special_flags;
                CALLBACK(SPECIAL, PUSH);
            }
            goto GT_NEXT;
        }

        GT_NEXT:
        continue;
    }
}

JSONSL_API
const char* jsonsl_strerror(jsonsl_error_t err)
{
#define X(t) \
    if (err == JSONSL_ERROR_##t) \
        return #t;
    JSONSL_XERR;
#undef X
    return "<UNKNOWN_ERROR>";
}

JSONSL_API
const char *jsonsl_strtype(jsonsl_type_t type)
{
#define X(o,c) \
    if (type == JSONSL_T_##o) \
        return #o;
    JSONSL_XTYPE
#undef X
    return "UNKNOWN TYPE";

}

/*
 *
 * JPR/JSONPointer functions
 *
 *
 */
#ifndef JSONSL_NO_JPR
static
jsonsl_jpr_type_t
populate_component(char *in,
                   struct jsonsl_jpr_component_st *component,
                   char **next,
                   jsonsl_error_t *errp)
{
    unsigned long pctval;
    char *c = NULL, *outp = NULL, *end = NULL;
    size_t input_len;
    jsonsl_jpr_type_t ret = JSONSL_PATH_NONE;

    if (*next == NULL || *(*next) == '\0') {
        return JSONSL_PATH_NONE;
    }

    /* Replace the next / with a NULL */
    *next = strstr(in, "/");
    if (*next != NULL) {
        *(*next) = '\0'; /* drop the forward slash */
        input_len = *next - in;
        end = *next;
        *next += 1; /* next character after the '/' */
    } else {
        input_len = strlen(in);
        end = in + input_len + 1;
    }

    component->pstr = in;

    /* Check for special components of interest */
    if (*in == JSONSL_PATH_WILDCARD_CHAR && input_len == 1) {
        /* Lone wildcard */
        ret = JSONSL_PATH_WILDCARD;
        goto GT_RET;
    } else if (isdigit(*in)) {
        /* ASCII Numeric */
        char *endptr;
        component->idx = strtoul(in, &endptr, 10);
        if (endptr && *endptr == '\0') {
            ret = JSONSL_PATH_NUMERIC;
            goto GT_RET;
        }
    }

    /* Default, it's a string */
    ret = JSONSL_PATH_STRING;
    for (c = outp = in; c < end; c++, outp++) {
        char origc;
        if (*c != '%') {
            goto GT_ASSIGN;
        }
        /*
         * c = { [+0] = '%', [+1] = 'b', [+2] = 'e', [+3] = '\0' }
         */

        /* Need %XX */
        if (c+2 >= end) {
            *errp = JSONSL_ERROR_PERCENT_BADHEX;
            return JSONSL_PATH_INVALID;
        }
        if (! (isxdigit(*(c+1)) && isxdigit(*(c+2))) ) {
            *errp = JSONSL_ERROR_PERCENT_BADHEX;
            return JSONSL_PATH_INVALID;
        }

        /* Temporarily null-terminate the characters */
        origc = *(c+3);
        *(c+3) = '\0';
        pctval = strtoul(c+1, NULL, 16);
        *(c+3) = origc;

        *outp = pctval;
        c += 2;
        continue;

        GT_ASSIGN:
        *outp = *c;
    }
    /* Null-terminate the string */
    for (; outp < c; outp++) {
        *outp = '\0';
    }

    GT_RET:
    component->ptype = ret;
    if (ret != JSONSL_PATH_WILDCARD) {
        component->len = strlen(component->pstr);
    }
    return ret;
}

JSONSL_API
jsonsl_jpr_t
jsonsl_jpr_new(const char *path, jsonsl_error_t *errp)
{
    char *my_copy;
    int count, curidx;
    struct jsonsl_jpr_st *ret;
    struct jsonsl_jpr_component_st *components;
    size_t origlen;
    jsonsl_error_t errstacked;

    if (errp == NULL) {
        errp = &errstacked;
    }

    if (path == NULL || *path != '/') {
        *errp = JSONSL_ERROR_JPR_NOROOT;
        return NULL;
    }

    count = 1;
    path++;
    {
        const char *c = path;
        for (; *c; c++) {
            if (*c == '/') {
                count++;
                if (*(c+1) == '/') {
                    *errp = JSONSL_ERROR_JPR_DUPSLASH;
                    return NULL;
                }
            }
        }
    }
    if(*path) {
        count++;
    }

    components = malloc(sizeof(*components) * count);
    my_copy = malloc(strlen(path) + 1);
    strcpy(my_copy, path);

    components[0].ptype = JSONSL_PATH_ROOT;

    if (*my_copy) {
        char *cur = my_copy;
        int pathret = JSONSL_PATH_STRING;
        curidx = 1;
        while (pathret > 0 && curidx < count) {
            pathret = populate_component(cur, components + curidx, &cur, errp);
            if (pathret > 0) {
                curidx++;
            } else {
                break;
            }
        }

        if (pathret == JSONSL_PATH_INVALID) {
            free(components);
            free(my_copy);
            return NULL;
        }
    } else {
        curidx = 1;
    }

    path--; /*revert path to leading '/' */
    origlen = strlen(path) + 1;
    ret = malloc(sizeof(*ret));
    ret->components = components;
    ret->ncomponents = curidx;
    ret->basestr = my_copy;
    ret->orig = malloc(origlen);
    strcpy(ret->orig, path);

    return ret;
}

void jsonsl_jpr_destroy(jsonsl_jpr_t jpr)
{
    free(jpr->components);
    free(jpr->basestr);
    free(jpr->orig);
    free(jpr);
}

JSONSL_API
jsonsl_jpr_match_t
jsonsl_jpr_match(jsonsl_jpr_t jpr,
                   jsonsl_type_t parent_type,
                   unsigned int parent_level,
                   const char *key,
                   size_t nkey)
{
    /* find our current component. This is the child level */
    int cmpret;
    struct jsonsl_jpr_component_st *p_component;
    p_component = jpr->components + parent_level;

    if (parent_level >= jpr->ncomponents) {
        return JSONSL_MATCH_NOMATCH;
    }

    /* Lone query for 'root' element. Always matches */
    if (parent_level == 0) {
        if (jpr->ncomponents == 1) {
            return JSONSL_MATCH_COMPLETE;
        } else {
            return JSONSL_MATCH_POSSIBLE;
        }
    }

    /* Wildcard, always matches */
    if (p_component->ptype == JSONSL_PATH_WILDCARD) {
        if (parent_level == jpr->ncomponents-1) {
            return JSONSL_MATCH_COMPLETE;
        } else {
            return JSONSL_MATCH_POSSIBLE;
        }
    }

    /* Check numeric array index */
    if (p_component->ptype == JSONSL_PATH_NUMERIC
            && parent_type == JSONSL_T_LIST) {
        if (p_component->idx != nkey) {
            return JSONSL_MATCH_NOMATCH;
        } else {
            if (parent_level == jpr->ncomponents-1) {
                return JSONSL_MATCH_COMPLETE;
            } else {
                return JSONSL_MATCH_POSSIBLE;
            }
        }
    }

    /* Check lengths */
    if (p_component->len != nkey) {
        return JSONSL_MATCH_NOMATCH;
    }

    /* Check string comparison */
    cmpret = strncmp(p_component->pstr, key, nkey);
    if (cmpret != 0) {
        return JSONSL_MATCH_NOMATCH;
    } else {
        if (parent_level == jpr->ncomponents-1) {
            return JSONSL_MATCH_COMPLETE;
        } else {
            return JSONSL_MATCH_POSSIBLE;
        }
    }

    /* Never reached, but make the compiler happy */
    abort();
    return JSONSL_MATCH_NOMATCH;
}

JSONSL_API
void jsonsl_jpr_match_state_init(jsonsl_t jsn,
                                 jsonsl_jpr_t *jprs,
                                 size_t njprs)
{
    int ii, *firstjmp;
    if (njprs == 0) {
        return;
    }
    jsn->jprs = malloc(sizeof(jsonsl_jpr_t) * njprs);
    jsn->jpr_count = njprs;
    jsn->jpr_root = calloc(1, sizeof(int) * njprs * jsn->levels_max);
    memcpy(jsn->jprs, jprs, sizeof(jsonsl_jpr_t) * njprs);
    /* Set the initial jump table values */

    firstjmp = jsn->jpr_root;
    for (ii = 0; ii < njprs; ii++) {
        firstjmp[ii] = ii+1;
    }
}

JSONSL_API
void jsonsl_jpr_match_state_cleanup(jsonsl_t jsn)
{
    if (jsn->jpr_count == 0) {
        return;
    }

    free(jsn->jpr_root);
    free(jsn->jprs);
    jsn->jprs = NULL;
    jsn->jpr_root = NULL;
    jsn->jpr_count = 0;
}

/**
 * This function should be called exactly once on each element...
 * This should also be called in recursive order, since we rely
 * on the parent having been initalized for a match.
 *
 * Since the parent is checked for a match as well, we maintain a 'serial' counter.
 * Whenever we traverse an element, we expect the serial to be the same as a global
 * integer. If they do not match, we re-initialize the context, and set the serial.
 *
 * This ensures a type of consistency without having a proactive reset by the
 * main lexer itself.
 *
 */
JSONSL_API
jsonsl_jpr_t jsonsl_jpr_match_state(jsonsl_t jsn,
                                    struct jsonsl_state_st *state,
                                    const char *key,
                                    size_t nkey,
                                    jsonsl_jpr_match_t *out)
{
    struct jsonsl_state_st *parent_state;
    jsonsl_jpr_t ret = NULL;

    /* Jump and JPR tables for our own state and the parent state */
    int *jmptable, *pjmptable;
    int jmp_cur, ii, ourjmpidx;

    if (!jsn->jpr_root) {
        *out = JSONSL_MATCH_NOMATCH;
        return NULL;
    }

    pjmptable = jsn->jpr_root + (jsn->jpr_count * (state->level-1));
    jmptable = pjmptable + jsn->jpr_count;

    /* If the parent cannot match, then invalidate it */
    if (*pjmptable == 0) {
        *jmptable = 0;
        *out = JSONSL_MATCH_NOMATCH;
        return NULL;
    }

    parent_state = jsn->stack + state->level - 1;

    if (parent_state->type == JSONSL_T_LIST) {
        nkey = parent_state->nelem;
    }

    *jmptable = 0;
    ourjmpidx = 0;
    memset(jmptable, 0, sizeof(int) * jsn->jpr_count);

    for (ii = 0; ii <  jsn->jpr_count; ii++) {
        jmp_cur = pjmptable[ii];
        if (jmp_cur) {
            jsonsl_jpr_t jpr = jsn->jprs[jmp_cur-1];
            *out = jsonsl_jpr_match(jpr,
                                    parent_state->type,
                                    parent_state->level,
                                    key, nkey);
            if (*out == JSONSL_MATCH_COMPLETE) {
                ret = jpr;
                *jmptable = 0;
                return ret;
            } else if (*out == JSONSL_MATCH_POSSIBLE) {
                jmptable[ourjmpidx] = ii+1;
                ourjmpidx++;
            }
        } else {
            break;
        }
    }
    if (!*jmptable) {
        *out = JSONSL_MATCH_NOMATCH;
    }
    return NULL;
}

JSONSL_API
const char *jsonsl_strmatchtype(jsonsl_jpr_match_t match)
{
#define X(T,v) \
    if ( match == JSONSL_MATCH_##T ) \
        return #T;
    JSONSL_XMATCH
#undef X
    return "<UNKNOWN>";
}

#endif /* JSONSL_WITH_JPR */

/**
 * Character Table definitions.
 * These were all generated via srcutil/genchartables.pl
 */

/**
 * This table contains the beginnings of non-string
 * allowable (bareword) values.
 */
static jsonsl_special_t _special_table[0xff] = {
        /* 0x00 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0x1f */
        /* 0x20 */ 0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0x2c */
        /* 0x2d */ JSONSL_SPECIALf_SIGNED /* - */, /* 0x2d */
        /* 0x2e */ 0,0, /* 0x2f */
        /* 0x30 */ JSONSL_SPECIALf_UNSIGNED /* 0 */, /* 0x30 */
        /* 0x31 */ JSONSL_SPECIALf_UNSIGNED /* 1 */, /* 0x31 */
        /* 0x32 */ JSONSL_SPECIALf_UNSIGNED /* 2 */, /* 0x32 */
        /* 0x33 */ JSONSL_SPECIALf_UNSIGNED /* 3 */, /* 0x33 */
        /* 0x34 */ JSONSL_SPECIALf_UNSIGNED /* 4 */, /* 0x34 */
        /* 0x35 */ JSONSL_SPECIALf_UNSIGNED /* 5 */, /* 0x35 */
        /* 0x36 */ JSONSL_SPECIALf_UNSIGNED /* 6 */, /* 0x36 */
        /* 0x37 */ JSONSL_SPECIALf_UNSIGNED /* 7 */, /* 0x37 */
        /* 0x38 */ JSONSL_SPECIALf_UNSIGNED /* 8 */, /* 0x38 */
        /* 0x39 */ JSONSL_SPECIALf_UNSIGNED /* 9 */, /* 0x39 */
        /* 0x3a */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0x59 */
        /* 0x5a */ 0,0,0,0,0,0,0,0,0,0,0,0, /* 0x65 */
        /* 0x66 */ JSONSL_SPECIALf_FALSE /* f */, /* 0x66 */
        /* 0x67 */ 0,0,0,0,0,0,0, /* 0x6d */
        /* 0x6e */ JSONSL_SPECIALf_NULL /* n */, /* 0x6e */
        /* 0x6f */ 0,0,0,0,0, /* 0x73 */
        /* 0x74 */ JSONSL_SPECIALf_TRUE /* t */, /* 0x74 */
        /* 0x75 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0x94 */
        /* 0x95 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0xb4 */
        /* 0xb5 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0xd4 */
        /* 0xd5 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0xf4 */
        /* 0xf5 */ 0,0,0,0,0,0,0,0,0,0 /* 0xfe */
};
static jsonsl_special_t *Special_table = _special_table;

/**
 * Contains characters which signal the termination of any of the 'special' bareword
 * values.
 */
static int _special_endings[0xff] = {
        /* 0x00 */ 0,0,0,0,0,0,0,0,0, /* 0x08 */
        /* 0x09 */ 1 /* <TAB> */, /* 0x09 */
        /* 0x0a */ 1 /* <LF> */, /* 0x0a */
        /* 0x0b */ 0,0, /* 0x0c */
        /* 0x0d */ 1 /* <CR> */, /* 0x0d */
        /* 0x0e */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0x1f */
        /* 0x20 */ 1 /* <SP> */, /* 0x20 */
        /* 0x21 */ 0, /* 0x21 */
        /* 0x22 */ 1 /* " */, /* 0x22 */
        /* 0x23 */ 0,0,0,0,0,0,0,0,0, /* 0x2b */
        /* 0x2c */ 1 /* , */, /* 0x2c */
        /* 0x2d */ 0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0x39 */
        /* 0x3a */ 1 /* : */, /* 0x3a */
        /* 0x3b */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0x5a */
        /* 0x5b */ 1 /* [ */, /* 0x5b */
        /* 0x5c */ 1 /* \ */, /* 0x5c */
        /* 0x5d */ 1 /* ] */, /* 0x5d */
        /* 0x5e */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0x7a */
        /* 0x7b */ 1 /* { */, /* 0x7b */
        /* 0x7c */ 0, /* 0x7c */
        /* 0x7d */ 1 /* } */, /* 0x7d */
        /* 0x7e */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0x9d */
        /* 0x9e */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0xbd */
        /* 0xbe */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0xdd */
        /* 0xde */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0xfd */
        /* 0xfe */ 0 /* 0xfe */
};
static int *Special_Endings = _special_endings;

/**
 * Contains allowable whitespace.
 */
static int _allowed_whitespace[0xff] = {
        /* 0x00 */ 0,0,0,0,0,0,0,0,0, /* 0x08 */
        /* 0x09 */ 1 /* <TAB> */, /* 0x09 */
        /* 0x0a */ 1 /* <LF> */, /* 0x0a */
        /* 0x0b */ 0,0, /* 0x0c */
        /* 0x0d */ 1 /* <CR> */, /* 0x0d */
        /* 0x0e */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0x1f */
        /* 0x20 */ 1 /* <SP> */, /* 0x20 */
        /* 0x21 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0x40 */
        /* 0x41 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0x60 */
        /* 0x61 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0x80 */
        /* 0x81 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0xa0 */
        /* 0xa1 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0xc0 */
        /* 0xc1 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0xe0 */
        /* 0xe1 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 /* 0xfe */
};
static int *Allowed_Whitespace = _allowed_whitespace;
