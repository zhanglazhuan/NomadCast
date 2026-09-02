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

#ifndef cJSON__h
#define cJSON__h

#ifdef __cplusplus
extern "C" {
#endif

/* cJSON Types: */
#define cJSON_Invalid (0)
#define cJSON_False   (1 << 0)
#define cJSON_True    (1 << 1)
#define cJSON_NULL    (1 << 2)
#define cJSON_Number  (1 << 3)
#define cJSON_String  (1 << 4)
#define cJSON_Array   (1 << 5)
#define cJSON_Object  (1 << 6)
#define cJSON_Raw     (1 << 7) /* raw json */

#define cJSON_IsReference 256
#define cJSON_StringIsConst 512

/* The cJSON structure: */
typedef struct cJSON
{
    struct cJSON *next, *prev;  /* next/prev allow you to walk array/object chains */
    struct cJSON *child;        /* An array or object item will have a child pointer pointing to a chain of the items in the array/object. */

    int type;                   /* The type of the item, as above. */

    char *valuestring;          /* The item's string, if type==cJSON_String */
    int valueint;               /* The item's number, if type==cJSON_Number */
    double valuedouble;         /* The item's number, if type==cJSON_Number */

    char *string;               /* The item's name string, if this item is the child of, or is in the list of subitems of an object. */
} cJSON;

typedef struct cJSON_Hooks
{
    void *(*malloc_fn)(size_t sz);
    void (*free_fn)(void *ptr);
} cJSON_Hooks;

/* Supply malloc, realloc and free functions to cJSON */
void cJSON_InitHooks(cJSON_Hooks* hooks);

/* Memory Management: the caller is always responsible to free the results from all variants of cJSON_Parse (with cJSON_Delete) and cJSON_Print (with stdlib free). */
void cJSON_Delete(cJSON *item);

/* Parse an object - create a new root, and populate. */
cJSON *cJSON_Parse(const char *value);
cJSON *cJSON_ParseWithLength(const char *value, size_t buffer_length);

/* Render a JSON entity to text for transfer/storage. */
char *cJSON_Print(const cJSON *item);
char *cJSON_PrintUnformatted(const cJSON *item);
char *cJSON_PrintBuffered(const cJSON *item, int prebuffer, int fmt);

/* Delete a cJSON entity and all subentities. */
void   cJSON_Delete(cJSON *item);

/* Returns the number of items in an array (or object). */
int    cJSON_GetArraySize(const cJSON *item);

/* Retrieve item number "index" from array "array". */
cJSON *cJSON_GetArrayItem(const cJSON *array, int index);

/* Get item "string" from object. Case insensitive. */
cJSON *cJSON_GetObjectItem(const cJSON *object, const char *string);
cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON * const object, const char * const string);
int    cJSON_HasObjectItem(const cJSON *object, const char *string);

/* For analysing failed parses. This returns a pointer to the parse error. You'll probably need to look a few chars back to make sense of it. Defined when cJSON_Parse() returns 0. 0 when cJSON_Parse() succeeds. */
const char *cJSON_GetErrorPtr(void);

/* Check item type and return its value */
char  *cJSON_GetStringValue(const cJSON * const item);
double cJSON_GetNumberValue(const cJSON * const item);

/* These functions check the type of an item */
int cJSON_IsInvalid(const cJSON * const item);
int cJSON_IsFalse(const cJSON * const item);
int cJSON_IsTrue(const cJSON * const item);
int cJSON_IsBool(const cJSON * const item);
int cJSON_IsNull(const cJSON * const item);
int cJSON_IsNumber(const cJSON * const item);
int cJSON_IsString(const cJSON * const item);
int cJSON_IsArray(const cJSON * const item);
int cJSON_IsObject(const cJSON * const item);
int cJSON_IsRaw(const cJSON * const item);

/* Create basic types: */
cJSON *cJSON_CreateNull(void);
cJSON *cJSON_CreateTrue(void);
cJSON *cJSON_CreateFalse(void);
cJSON *cJSON_CreateBool(int boolean);
cJSON *cJSON_CreateNumber(double num);
cJSON *cJSON_CreateString(const char *string);
cJSON *cJSON_CreateRaw(const char *raw);
cJSON *cJSON_CreateArray(void);
cJSON *cJSON_CreateObject(void);

/* Create arrays/objects with convenience helpers. They are also used to add structs where this is recursive. */
cJSON *cJSON_CreateIntArray(const int *numbers, int count);
cJSON *cJSON_CreateFloatArray(const float *numbers, int count);
cJSON *cJSON_CreateDoubleArray(const double *numbers, int count);
cJSON *cJSON_CreateStringArray(const char *const *strings, int count);

/* Append item to the specified array/object. */
cJSON *cJSON_AddItemToArray(cJSON *array, cJSON *item);
cJSON *cJSON_AddItemToObject(cJSON *object, const char *string, cJSON *item);

/* Append reference to item to the specified array/object. */
cJSON *cJSON_AddItemReferenceToArray(cJSON *array, cJSON *item);
cJSON *cJSON_AddItemReferenceToObject(cJSON *object, const char *string, cJSON *item);

/* Remove/Detach items from Arrays/Objects. */
cJSON *cJSON_DetachItemFromArray(cJSON *array, int which);
void   cJSON_DeleteItemFromArray(cJSON *array, int which);
cJSON *cJSON_DetachItemFromObject(cJSON *object, const char *string);
void   cJSON_DeleteItemFromObject(cJSON *object, const char *string);

/* Update array items. */
void cJSON_ReplaceItemInArray(cJSON *array, int which, cJSON *newitem);
void cJSON_ReplaceItemInObject(cJSON *object, const char *string, cJSON *newitem);

/* Duplicate a cJSON item */
cJSON *cJSON_Duplicate(const cJSON *item, int recurse);

/* ParseWithOpts allows you to require (and check) that the JSON is null terminated, and to retrieve the pointer to the final byte parsed. */
cJSON *cJSON_ParseWithOpts(const char *value, const char **return_parse_end, int require_null_terminated);

/* Macros for creating things quickly. */
#define cJSON_AddNullToObject(object,name)      cJSON_AddItemToObject(object, name, cJSON_CreateNull())
#define cJSON_AddTrueToObject(object,name)      cJSON_AddItemToObject(object, name, cJSON_CreateTrue())
#define cJSON_AddFalseToObject(object,name)     cJSON_AddItemToObject(object, name, cJSON_CreateFalse())
#define cJSON_AddBoolToObject(object,name,b)    cJSON_AddItemToObject(object, name, cJSON_CreateBool(b))
#define cJSON_AddNumberToObject(object,name,n)  cJSON_AddItemToObject(object, name, cJSON_CreateNumber(n))
#define cJSON_AddStringToObject(object,name,s)  cJSON_AddItemToObject(object, name, cJSON_CreateString(s))

#define cJSON_SetIntValue(object, number)       ((object) ? (object)->valueint = (object)->valuedouble = (number) : (number))
#define cJSON_SetNumberValue(object, number)    ((object) ? (object)->valueint = (int)(number), (object)->valuedouble = (number) : (number))

#ifdef __cplusplus
}
#endif

#endif
