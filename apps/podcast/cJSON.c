/*
  Copyright (c) 2009-2017 Dave Gamble and cJSON contributors

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
  THE SOFTWARE.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <limits.h>
#include <ctype.h>

#include "cJSON.h"

/* ── Internal helpers ────────────────────────────────────────────────────── */

static const unsigned char *global_ep = NULL;

const char *cJSON_GetErrorPtr(void) { return (const char *)global_ep; }

static void *(*cJSON_malloc)(size_t sz) = malloc;
static void (*cJSON_free)(void *ptr) = free;

void cJSON_InitHooks(cJSON_Hooks *hooks)
{
    if (hooks) {
        cJSON_malloc = (hooks->malloc_fn) ? hooks->malloc_fn : malloc;
        cJSON_free   = (hooks->free_fn)   ? hooks->free_fn   : free;
    }
}

/* internal constructor helper */
static cJSON *cJSON_New_Item(void)
{
    cJSON *node = (cJSON *)cJSON_malloc(sizeof(cJSON));
    if (node) memset(node, 0, sizeof(cJSON));
    return node;
}

/* delete a cJSON structure */
void cJSON_Delete(cJSON *c)
{
    cJSON *next;
    while (c) {
        next = c->next;
        if (!(c->type & cJSON_IsReference) && c->child) cJSON_Delete(c->child);
        if (!(c->type & cJSON_IsReference) && c->valuestring) cJSON_free(c->valuestring);
        if (!(c->type & cJSON_StringIsConst) && c->string) cJSON_free(c->string);
        cJSON_free(c);
        c = next;
    }
}

/* Parse the input text to generate a number. */
static const unsigned char *parse_number(cJSON *item, const unsigned char *num)
{
    double n = 0, sign = 1, scale = 0;
    int subscale = 0, signsubscale = 1;

    if (*num == '-') { sign = -1; num++; }
    if (*num == '0') num++;
    if (*num >= '1' && *num <= '9') {
        do { n = (n * 10.0) + (*num++ - '0'); }
        while (*num >= '0' && *num <= '9');
    }
    if (*num == '.') {
        num++;
        while (*num >= '0' && *num <= '9') {
            n = (n * 10.0) + (*num++ - '0'); scale--;
        }
    }
    if (*num == 'e' || *num == 'E') {
        num++;
        if (*num == '+') num++;
        else if (*num == '-') { signsubscale = -1; num++; }
        while (*num >= '0' && *num <= '9') { subscale = (subscale * 10) + (*num++ - '0'); }
    }
    n = sign * n * pow(10.0, (scale + subscale * signsubscale));
    item->valuedouble = n;
    item->valueint = (int)n;
    return num;
}

/* Parse hex (4 digits) for \uXXXX escape */
static unsigned parse_hex4(const unsigned char *str)
{
    unsigned h = 0;
    for (int i = 0; i < 4; i++) {
        unsigned char c = str[i];
        h <<= 4;
        if (c >= '0' && c <= '9') h |= (c - '0');
        else if (c >= 'A' && c <= 'F') h |= (c - 'A' + 10);
        else if (c >= 'a' && c <= 'f') h |= (c - 'a' + 10);
        else return 0;
    }
    return h;
}

/* Parse a string. */
static const unsigned char *parse_string(cJSON *item, const unsigned char *str)
{
    const unsigned char *ptr = str + 1;
    unsigned char *ptr2;
    unsigned char *out;
    int len = 0;

    if (*str != '\"') { global_ep = str; return 0; }

    /* First pass: calculate length */
    while (*ptr != '\"' && *ptr) {
        if (*ptr++ == '\\') { if (*ptr) ptr++; }
        len++;
    }
    if (!*ptr) { global_ep = str; return 0; }

    out = (unsigned char *)cJSON_malloc(len + 1);
    if (!out) return 0;

    ptr = str + 1;
    ptr2 = out;
    while (*ptr != '\"' && *ptr) {
        if (*ptr != '\\') {
            *ptr2++ = *ptr++;
        } else {
            ptr++;
            switch (*ptr) {
            case 'b':  *ptr2++ = '\b'; break;
            case 'f':  *ptr2++ = '\f'; break;
            case 'n':  *ptr2++ = '\n'; break;
            case 'r':  *ptr2++ = '\r'; break;
            case 't':  *ptr2++ = '\t'; break;
            case 'u':
                /* very basic unicode: store as UTF-8 for BMP */
                {
                    unsigned uc = parse_hex4(ptr + 1);
                    ptr += 4;
                    if (uc < 0x80) {
                        *ptr2++ = (unsigned char)uc;
                    } else if (uc < 0x800) {
                        *ptr2++ = 0xC0 | (uc >> 6);
                        *ptr2++ = 0x80 | (uc & 0x3F);
                    } else {
                        *ptr2++ = 0xE0 | (uc >> 12);
                        *ptr2++ = 0x80 | ((uc >> 6) & 0x3F);
                        *ptr2++ = 0x80 | (uc & 0x3F);
                    }
                }
                break;
            default:  *ptr2++ = *ptr; break;
            }
            ptr++;
        }
    }
    *ptr2 = 0;
    if (*ptr == '\"') ptr++;
    item->valuestring = (char *)out;
    return ptr;
}

/* Forward declaration */
static const unsigned char *parse_value(cJSON *item, const unsigned char *value);

/* Build a linked list from an array/object parse. */
static const unsigned char *parse_array(cJSON *item, const unsigned char *value)
{
    cJSON *child;
    if (*value != '[') { global_ep = value; return 0; }
    value++;

    /* skip whitespace */
    while (*value && (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n')) value++;

    if (*value == ']') return value + 1;

    child = cJSON_New_Item();
    if (!child) return 0;
    item->child = child;
    value = parse_value(child, value);
    if (!value) return 0;

    while (*value == ',') {
        cJSON *new_item = cJSON_New_Item();
        if (!new_item) return 0;
        child->next = new_item;
        new_item->prev = child;
        child = new_item;
        value++; /* skip comma */
        while (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n') value++;
        value = parse_value(child, value);
        if (!value) return 0;
    }

    if (*value == ']') return value + 1;
    global_ep = value; return 0;
}

static const unsigned char *parse_object(cJSON *item, const unsigned char *value)
{
    cJSON *child;
    if (*value != '{') { global_ep = value; return 0; }
    value++;

    while (*value && (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n')) value++;
    if (*value == '}') return value + 1;

    child = cJSON_New_Item();
    if (!child) return 0;
    item->child = child;
    value = parse_string(child, value);
    if (!value) return 0;

    /* parse_string puts the key in valuestring — move it to string */
    child->string = child->valuestring;
    child->valuestring = NULL;

    /* skip colon */
    while (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n') value++;
    if (*value != ':') { global_ep = value; return 0; }
    value++;

    while (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n') value++;
    value = parse_value(child, value);
    if (!value) return 0;

    while (*value == ',') {
        cJSON *new_item = cJSON_New_Item();
        if (!new_item) return 0;
        child->next = new_item;
        new_item->prev = child;
        child = new_item;
        value++; /* skip comma */
        while (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n') value++;
        value = parse_string(child, value);
        if (!value) return 0;
        /* move key from valuestring to string */
        child->string = child->valuestring;
        child->valuestring = NULL;
        while (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n') value++;
        if (*value != ':') { global_ep = value; return 0; }
        value++;
        while (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n') value++;
        value = parse_value(child, value);
        if (!value) return 0;
    }

    if (*value == '}') return value + 1;
    global_ep = value; return 0;
}

/* Parser core */
static const unsigned char *parse_value(cJSON *item, const unsigned char *value)
{
    if (!value) return 0;

    while (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n') value++;

    /* determine type */
    switch (*value) {
    case '\"': item->type = cJSON_String; return parse_string(item, value);
    case '{':  item->type = cJSON_Object; return parse_object(item, value);
    case '[':  item->type = cJSON_Array;  return parse_array(item, value);
    case 't':
        if (strncmp((const char *)value, "true", 4) == 0) {
            item->type = cJSON_True; return value + 4;
        }
        break;
    case 'f':
        if (strncmp((const char *)value, "false", 5) == 0) {
            item->type = cJSON_False; return value + 5;
        }
        break;
    case 'n':
        if (strncmp((const char *)value, "null", 4) == 0) {
            item->type = cJSON_NULL; return value + 4;
        }
        break;
    case '-':
    case '0': case '1': case '2': case '3': case '4':
    case '5': case '6': case '7': case '8': case '9':
        item->type = cJSON_Number;
        return parse_number(item, value);
    }

    global_ep = value;
    return 0;
}

/* ── Public parsing API ──────────────────────────────────────────────────── */

cJSON *cJSON_ParseWithOpts(const char *value, const char **return_parse_end,
                           int require_null_terminated)
{
    const unsigned char *end = NULL;
    cJSON *c = cJSON_New_Item();
    global_ep = NULL;
    if (!c) return NULL;

    end = parse_value(c, (const unsigned char *)value);
    if (!end) { cJSON_Delete(c); return NULL; }

    /* check if there's trailing non-whitespace garbage */
    if (require_null_terminated) {
        const unsigned char *p = end;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p != '\0') { cJSON_Delete(c); global_ep = end; return NULL; }
    }

    if (return_parse_end) *return_parse_end = (const char *)end;
    return c;
}

cJSON *cJSON_Parse(const char *value)
{
    return cJSON_ParseWithOpts(value, 0, 0);
}

cJSON *cJSON_ParseWithLength(const char *value, size_t buffer_length)
{
    /* We need a null-terminated string. Make a copy if needed. */
    char *copy = (char *)cJSON_malloc(buffer_length + 1);
    if (!copy) return NULL;
    memcpy(copy, value, buffer_length);
    copy[buffer_length] = '\0';
    cJSON *result = cJSON_Parse(copy);
    cJSON_free(copy);
    return result;
}

/* ── Rendering (print to string) ──────────────────────────────────────────── */

typedef struct {
    char   *buffer;
    size_t  length;
    size_t  offset;
} printbuffer;

static int ensure(printbuffer *p, size_t need)
{
    size_t newlen = p->length;
    if (!p->buffer) newlen = 256;
    while (newlen < p->offset + need + 1) newlen *= 2;
    if (newlen > p->length) {
        char *nb = (char *)realloc(p->buffer, newlen);
        if (!nb) return 0;
        p->buffer = nb;
        p->length = newlen;
    }
    return 1;
}

static void sprint(printbuffer *p, const char *str)
{
    size_t len = strlen(str);
    if (ensure(p, len)) {
        memcpy(p->buffer + p->offset, str, len);
        p->offset += len;
        p->buffer[p->offset] = '\0';
    }
}

/* Render number */
static int print_number(cJSON *item, printbuffer *p)
{
    char str[64];
    double d = item->valuedouble;
    if (d == (double)((int)d) && d < 1e9 && d > -1e9) {
        sprintf(str, "%d", item->valueint);
    } else {
        sprintf(str, "%g", d);
    }
    sprint(p, str);
    return 1;
}

/* Forward */
static int print_value(cJSON *item, int depth, int fmt, printbuffer *p);

/* Render string (with escaping) */
static int print_string(cJSON *item, printbuffer *p)
{
    const char *str = item->valuestring;
    if (!str) { sprint(p, "\"\""); return 1; }
    if (!ensure(p, strlen(str) * 2 + 3)) return 0;
    sprint(p, "\"");
    while (*str) {
        switch (*str) {
        case '\"': sprint(p, "\\\""); break;
        case '\\': sprint(p, "\\\\"); break;
        case '\b': sprint(p, "\\b"); break;
        case '\f': sprint(p, "\\f"); break;
        case '\n': sprint(p, "\\n"); break;
        case '\r': sprint(p, "\\r"); break;
        case '\t': sprint(p, "\\t"); break;
        default:
            if ((unsigned char)*str < 32) {
                char buf[8];
                sprintf(buf, "\\u%04x", (unsigned char)*str);
                sprint(p, buf);
            } else {
                char buf[2] = {*str, 0};
                sprint(p, buf);
            }
            break;
        }
        str++;
    }
    sprint(p, "\"");
    return 1;
}

static int print_array(cJSON *item, int depth, int fmt, printbuffer *p)
{
    cJSON *child = item->child;
    if (!child) { sprint(p, "[]"); return 1; }
    sprint(p, "[");
    if (fmt) { sprint(p, "\n"); }
    while (child) {
        if (fmt) {
            for (int i = 0; i < depth + 1; i++) sprint(p, "  ");
        }
        print_value(child, depth + 1, fmt, p);
        if (child->next) sprint(p, ",");
        if (fmt) sprint(p, "\n");
        child = child->next;
    }
    if (fmt) {
        for (int i = 0; i < depth; i++) sprint(p, "  ");
    }
    sprint(p, "]");
    return 1;
}

static int print_object(cJSON *item, int depth, int fmt, printbuffer *p)
{
    cJSON *child = item->child;
    if (!child) { sprint(p, "{}"); return 1; }
    sprint(p, "{");
    if (fmt) { sprint(p, "\n"); }
    while (child) {
        if (fmt) {
            for (int i = 0; i < depth + 1; i++) sprint(p, "  ");
        }
        print_string(child, p);
        sprint(p, ":");
        if (fmt) sprint(p, " ");
        print_value(child, depth + 1, fmt, p);
        if (child->next) sprint(p, ",");
        if (fmt) sprint(p, "\n");
        child = child->next;
    }
    if (fmt) {
        for (int i = 0; i < depth; i++) sprint(p, "  ");
    }
    sprint(p, "}");
    return 1;
}

static int print_value(cJSON *item, int depth, int fmt, printbuffer *p)
{
    if (!item) return 0;
    switch (item->type) {
    case cJSON_Invalid: return 0;
    case cJSON_False:   sprint(p, "false"); break;
    case cJSON_True:    sprint(p, "true"); break;
    case cJSON_NULL:    sprint(p, "null"); break;
    case cJSON_Number:  print_number(item, p); break;
    case cJSON_String:  print_string(item, p); break;
    case cJSON_Array:   print_array(item, depth, fmt, p); break;
    case cJSON_Object:  print_object(item, depth, fmt, p); break;
    }
    return 1;
}

char *cJSON_Print(const cJSON *item)
{
    return cJSON_PrintBuffered(item, 256, 1);
}

char *cJSON_PrintUnformatted(const cJSON *item)
{
    return cJSON_PrintBuffered(item, 256, 0);
}

char *cJSON_PrintBuffered(const cJSON *item, int prebuffer, int fmt)
{
    printbuffer p;
    memset(&p, 0, sizeof(p));
    p.buffer = (char *)cJSON_malloc(prebuffer);
    if (!p.buffer) return NULL;
    p.buffer[0] = '\0';
    p.length = prebuffer;
    p.offset = 0;
    if (!print_value((cJSON *)item, 0, fmt, &p)) {
        cJSON_free(p.buffer);
        return NULL;
    }
    return p.buffer;
}

/* ── Getter helpers ───────────────────────────────────────────────────────── */

int cJSON_GetArraySize(const cJSON *item)
{
    int count = 0;
    cJSON *child;
    if (!item) return 0;
    for (child = item->child; child; child = child->next) count++;
    return count;
}

cJSON *cJSON_GetArrayItem(const cJSON *array, int index)
{
    cJSON *child;
    int i = 0;
    if (!array) return NULL;
    for (child = array->child; child; child = child->next) {
        if (i == index) return child;
        i++;
    }
    return NULL;
}

cJSON *cJSON_GetObjectItem(const cJSON *object, const char *string)
{
    cJSON *child;
    if (!object || !string) return NULL;
    for (child = object->child; child; child = child->next) {
        if (child->string && strcasecmp(child->string, string) == 0)
            return child;
    }
    return NULL;
}

cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *object, const char *string)
{
    cJSON *child;
    if (!object || !string) return NULL;
    for (child = object->child; child; child = child->next) {
        if (child->string && strcmp(child->string, string) == 0)
            return child;
    }
    return NULL;
}

int cJSON_HasObjectItem(const cJSON *object, const char *string)
{
    return cJSON_GetObjectItem(object, string) ? 1 : 0;
}

char *cJSON_GetStringValue(const cJSON *item)
{
    if (!item || item->type != cJSON_String) return NULL;
    return item->valuestring;
}

double cJSON_GetNumberValue(const cJSON *item)
{
    if (!item || item->type != cJSON_Number) return 0.0;
    return item->valuedouble;
}

/* ── Type checkers ────────────────────────────────────────────────────────── */

int cJSON_IsInvalid(const cJSON *item) { return (item == NULL) || (item->type == cJSON_Invalid); }
int cJSON_IsFalse(const cJSON *item)   { return (item != NULL) && (item->type == cJSON_False); }
int cJSON_IsTrue(const cJSON *item)    { return (item != NULL) && (item->type == cJSON_True); }
int cJSON_IsBool(const cJSON *item)    { return cJSON_IsFalse(item) || cJSON_IsTrue(item); }
int cJSON_IsNull(const cJSON *item)    { return (item != NULL) && (item->type == cJSON_NULL); }
int cJSON_IsNumber(const cJSON *item)  { return (item != NULL) && (item->type == cJSON_Number); }
int cJSON_IsString(const cJSON *item)  { return (item != NULL) && (item->type == cJSON_String); }
int cJSON_IsArray(const cJSON *item)   { return (item != NULL) && (item->type == cJSON_Array); }
int cJSON_IsObject(const cJSON *item)  { return (item != NULL) && (item->type == cJSON_Object); }
int cJSON_IsRaw(const cJSON *item)     { return (item != NULL) && (item->type == cJSON_Raw); }

/* ── Constructors ─────────────────────────────────────────────────────────── */

static cJSON *cJSON_CreateSimple(int type)
{
    cJSON *item = cJSON_New_Item();
    if (item) item->type = type;
    return item;
}

cJSON *cJSON_CreateNull(void)   { return cJSON_CreateSimple(cJSON_NULL); }
cJSON *cJSON_CreateTrue(void)   { return cJSON_CreateSimple(cJSON_True); }
cJSON *cJSON_CreateFalse(void)  { return cJSON_CreateSimple(cJSON_False); }
cJSON *cJSON_CreateBool(int b)  { return cJSON_CreateSimple(b ? cJSON_True : cJSON_False); }

cJSON *cJSON_CreateNumber(double num)
{
    cJSON *item = cJSON_CreateSimple(cJSON_Number);
    if (item) { item->valuedouble = num; item->valueint = (int)num; }
    return item;
}

cJSON *cJSON_CreateString(const char *string)
{
    cJSON *item = cJSON_New_Item();
    if (!item) return NULL;
    item->type = cJSON_String;
    if (string) {
        item->valuestring = (char *)cJSON_malloc(strlen(string) + 1);
        if (!item->valuestring) { cJSON_free(item); return NULL; }
        strcpy(item->valuestring, string);
    }
    return item;
}

cJSON *cJSON_CreateRaw(const char *raw)
{
    cJSON *item = cJSON_CreateSimple(cJSON_Raw);
    if (item && raw) {
        item->valuestring = (char *)cJSON_malloc(strlen(raw) + 1);
        if (item->valuestring) strcpy(item->valuestring, raw);
    }
    return item;
}

cJSON *cJSON_CreateArray(void)  { return cJSON_CreateSimple(cJSON_Array); }
cJSON *cJSON_CreateObject(void) { return cJSON_CreateSimple(cJSON_Object); }

/* ── Append / modify ──────────────────────────────────────────────────────── */

cJSON *cJSON_AddItemToArray(cJSON *array, cJSON *item)
{
    cJSON *child;
    if (!array || !item) return NULL;
    if (!array->child) {
        array->child = item;
    } else {
        for (child = array->child; child->next; child = child->next);
        child->next = item;
        item->prev = child;
    }
    return item;
}

cJSON *cJSON_AddItemToObject(cJSON *object, const char *string, cJSON *item)
{
    if (!object || !string || !item) return NULL;
    if (item->string) cJSON_free(item->string);
    item->string = (char *)cJSON_malloc(strlen(string) + 1);
    if (!item->string) return NULL;
    strcpy(item->string, string);
    return cJSON_AddItemToArray(object, item);
}

cJSON *cJSON_AddItemReferenceToArray(cJSON *array, cJSON *item)
{
    if (!array || !item) return NULL;
    cJSON *ref = cJSON_New_Item();
    if (!ref) return NULL;
    memcpy(ref, item, sizeof(cJSON));
    ref->type |= cJSON_IsReference;
    ref->next = NULL; ref->prev = NULL;
    return cJSON_AddItemToArray(array, ref);
}

cJSON *cJSON_AddItemReferenceToObject(cJSON *object, const char *string, cJSON *item)
{
    if (!object || !string || !item) return NULL;
    cJSON *ref = cJSON_New_Item();
    if (!ref) return NULL;
    memcpy(ref, item, sizeof(cJSON));
    ref->type |= cJSON_IsReference;
    ref->next = NULL; ref->prev = NULL;
    ref->string = (char *)cJSON_malloc(strlen(string) + 1);
    if (!ref->string) { cJSON_free(ref); return NULL; }
    strcpy(ref->string, string);
    return cJSON_AddItemToArray(object, ref);
}

cJSON *cJSON_DetachItemFromArray(cJSON *array, int which)
{
    cJSON *child = cJSON_GetArrayItem(array, which);
    if (!child) return NULL;
    if (child->prev) child->prev->next = child->next;
    if (child->next) child->next->prev = child->prev;
    if (array->child == child) array->child = child->next;
    child->prev = child->next = NULL;
    return child;
}

void cJSON_DeleteItemFromArray(cJSON *array, int which)
{
    cJSON *child = cJSON_DetachItemFromArray(array, which);
    if (child) cJSON_Delete(child);
}

cJSON *cJSON_DetachItemFromObject(cJSON *object, const char *string)
{
    cJSON *child;
    if (!object || !string) return NULL;
    for (child = object->child; child; child = child->next) {
        if (child->string && strcmp(child->string, string) == 0) break;
    }
    if (!child) return NULL;
    if (child->prev) child->prev->next = child->next;
    if (child->next) child->next->prev = child->prev;
    if (object->child == child) object->child = child->next;
    child->prev = child->next = NULL;
    return child;
}

void cJSON_DeleteItemFromObject(cJSON *object, const char *string)
{
    cJSON *child = cJSON_DetachItemFromObject(object, string);
    if (child) cJSON_Delete(child);
}
