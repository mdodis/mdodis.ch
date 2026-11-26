/*
 * docubuild2 <Directory> -o <Output Directory> -m webpage|document
 *
 * Every .docuspec file may declare a top-level @prototype("webpage" | "document" | "any"), "any" when absent:
 *   webpage   only built in webpage mode; may use everything.
 *   document  built in both modes; webpage-only directives (raw html, interactivity) outside of
 *             @if_prototype("webpage") { ... } are errors.
 *   any       built in both modes; webpage-only directives are dropped (with a warning) from document builds.
 *
 * @if_prototype("webpage" | "document" | "any") { ... } renders its children only in that build mode.
 *
 * Each page is wrapped in <Template Directory>/<mode>.html, where <Template Directory> is <Directory>/templates
 * unless overridden with -t, see the TEMPLATES section.
 */
#define VD_USE_CRT 1
#define VD_MACRO_ABBREVIATIONS 1
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "vd.h"
#include "vd_docuspec.h"

#define PUT_LINE(fmt, ...) fprintf(out, fmt "\n", ## __VA_ARGS__)
#define PUT(fmt, ...) fprintf(out, fmt , ## __VA_ARGS__)

#define ERR_EXIT(fmt, ...) do { \
        fprintf(stderr, fmt, ## __VA_ARGS__); \
        exit(-1);                          \
    } while (0)

// Used both for a file's @prototype and for the build mode (which is only ever WEBPAGE or DOCUMENT).
typedef enum {
    PROTOTYPE_ANY = 0,
    PROTOTYPE_WEBPAGE,
    PROTOTYPE_DOCUMENT,
} Prototype;

typedef struct {
    Str         id;
    void        (*process)(VdDspcSection*, FILE*, int);
    Prototype   prototype;                                      // PROTOTYPE_WEBPAGE: skipped in document builds
} Processor;

typedef struct {
    Str         file_path_relative_to_source_dir;
    Str         name;                                           // Path without extension, output file stem
    Str         title;                                          // @title
    Str         category;                                       // @category
    Str         date;                                           // @date, YYYY-MM-DD
    int         notoc;                                          // @notoc
    Prototype   prototype;                                      // @prototype
    int         build;                                          // Produced by the current build mode
} SourceFile;

typedef struct {
    VdDspcSection *section;
    int           id;
} IndexedSection;

typedef struct {
    Arena               global_arena;
    Str                 source_dir;
    dynarray SourceFile *source_files;
    VdDspcDocument      document;
    SourceFile          *current_source_file;
    FILE                *search_index_file;                     // NULL in document builds
    int                 search_index_id;
    int                 search_index_entry_count;
    int                 is_prod;
    Prototype           mode;
    Str                 template_path;
    Str                 template_str;
    int                 error_count;
    int                 warning_count;
} Workspace;

typedef struct {
    VdDspcTree  *tree;
    SourceFile  *source_file;
    SourceFile  *doc;                                           // Current {{#docs}} item, NULL outside of it
} TemplateScope;

typedef struct {
    size_t      open;                                           // Offset of "{{"
    size_t      close;                                          // Offset just past "}}"
    char        kind;                                           // '#', '^', '/' or 0 for a variable
    Str         name;
} TemplateTag;

static Str make_str_from_section_id(VdDspcSection *section);
static Str make_str_from_tag_name(VdDspcTag *tag);
static Str make_str_from_tag_value(VdDspcTag *tag);
static Str make_str_from_str_node(VdDspcStrNode *node);
static Workspace *get_workspace(void);
static Processor *find_processor(Str section_id);
static int  is_rendered(VdDspcSection *section);
static int  if_prototype_matches(VdDspcSection *section);
static void process_child(VdDspcSection *section, FILE *out, int depth);
static void process_children(VdDspcSection *section, FILE *out, int depth);
static void process_verbatim_html(VdDspcSection *section, FILE *out, int depth);
static void search_index_begin_entry(FILE *out);
static void search_index_end_entry(FILE *out);
static int  search_index_id(void);
static int  search_index_curr_id(void);
static void put_json_str(FILE *out, Str s);
static Str  display_title(SourceFile *source_file);


static void process_apigen(VdDspcSection *section, FILE *out, int depth);
static void process_paste(VdDspcSection *section, FILE *out, int depth);
static void process_api_remarks(VdDspcSection *section, FILE *out, int depth);
static void process_api_params(VdDspcSection *section, FILE *out, int depth);
static void process_api_brief(VdDspcSection *section, FILE *out, int depth);
static void process_api_details(VdDspcSection *section, FILE *out, int depth);
static void process_api_decl(VdDspcSection *section, FILE *out, int depth);
static void process_api(VdDspcSection *section, FILE *out, int depth);
static void process_rev(VdDspcSection *section, FILE *out, int depth);
static void process_tblock(VdDspcSection *section, FILE *out, int depth);
static void process_carousel(VdDspcSection *section, FILE *out, int depth);
static void process_code(VdDspcSection *section, FILE *out, int depth);
static void process_copyright(VdDspcSection *section, FILE *out, int depth);
static void process_br(VdDspcSection *section, FILE *out, int depth);
static void process_accordion(VdDspcSection *section, FILE *out, int depth);
static void process_section(VdDspcSection *section, FILE *out, int depth);
static void process_text(VdDspcSection *section, FILE *out, int depth);
static void process_para(VdDspcSection *section, FILE *out, int depth);
static void process_list(VdDspcSection *section, FILE *out, int depth);
static void process_item(VdDspcSection *section, FILE *out, int depth);
static void process_if_prototype(VdDspcSection *section, FILE *out, int depth);

// Metadata (@title, @category, @date, @notoc, @prototype) is read up front by read_metadata and has no processor.
// Document builds keep content that any output format can represent (text, lists, sections, code, tables, images,
// api docs) and drop layout/interactive html.
static Processor Processor_Table[] = {
    {LIT_INLINE("paste"),             process_paste,            PROTOTYPE_WEBPAGE},
    {LIT_INLINE("api"),               process_api,              PROTOTYPE_ANY},
    {LIT_INLINE("api-decl"),          process_api_decl,         PROTOTYPE_ANY},
    {LIT_INLINE("api-details"),       process_api_details,      PROTOTYPE_ANY},
    {LIT_INLINE("api-brief"),         process_api_brief,        PROTOTYPE_ANY},
    {LIT_INLINE("api-params"),        process_api_params,       PROTOTYPE_ANY},
    {LIT_INLINE("api-remarks"),       process_api_remarks,      PROTOTYPE_ANY},
    {LIT_INLINE("copyright"),         process_copyright,        PROTOTYPE_ANY},
    {LIT_INLINE("br"),                process_br,               PROTOTYPE_ANY},
    {LIT_INLINE("accordion"),         process_accordion,        PROTOTYPE_WEBPAGE},
    {LIT_INLINE("carousel"),          process_carousel,         PROTOTYPE_WEBPAGE},
    {LIT_INLINE("section"),           process_section,          PROTOTYPE_ANY},
    {LIT_INLINE("apigen"),            process_apigen,           PROTOTYPE_ANY},
    {LIT_INLINE("text"),              process_text,             PROTOTYPE_ANY},
    {LIT_INLINE("para"),              process_para,             PROTOTYPE_ANY},
    {LIT_INLINE("verb"),              process_code,             PROTOTYPE_ANY},
    {LIT_INLINE("tblock"),            process_tblock,           PROTOTYPE_ANY},
    {LIT_INLINE("rev"),               process_rev,              PROTOTYPE_ANY},
    {LIT_INLINE("if_prototype"),      process_if_prototype,     PROTOTYPE_ANY},
    {LIT_INLINE("div"),               process_verbatim_html,    PROTOTYPE_WEBPAGE},
    {LIT_INLINE("span"),              process_verbatim_html,    PROTOTYPE_WEBPAGE},
    {LIT_INLINE("img"),               process_verbatim_html,    PROTOTYPE_ANY},
    {LIT_INLINE("h5"),                process_verbatim_html,    PROTOTYPE_WEBPAGE},
    {LIT_INLINE("p"),                 process_verbatim_html,    PROTOTYPE_WEBPAGE},
    {LIT_INLINE("a"),                 process_verbatim_html,    PROTOTYPE_WEBPAGE},
    {LIT_INLINE("table"),             process_verbatim_html,    PROTOTYPE_ANY},
    {LIT_INLINE("tr"),                process_verbatim_html,    PROTOTYPE_ANY},
    {LIT_INLINE("th"),                process_verbatim_html,    PROTOTYPE_ANY},
    {LIT_INLINE("td"),                process_verbatim_html,    PROTOTYPE_ANY},
    {LIT_INLINE("tbody"),             process_verbatim_html,    PROTOTYPE_ANY},
    {LIT_INLINE("button"),            process_verbatim_html,    PROTOTYPE_WEBPAGE},
    {LIT_INLINE("script"),            process_verbatim_html,    PROTOTYPE_WEBPAGE},
    {LIT_INLINE("list"),              process_list,             PROTOTYPE_ANY},
    {LIT_INLINE("item"),              process_item,             PROTOTYPE_ANY},
};

static void parse_and_generate_tree(FILE *f, FILE *out, Arena *temp_arena);

static void process_apigen(VdDspcSection *section, FILE *out, int depth)
{
    Arena *temp_arena = VD_GET_SCRATCH_ARENA();

    Str file_str = make_str_from_tag_value(vd_dspc_section_first_tag(section));

    StrBuilder bld;
    str_builder_init(&bld, temp_arena);
    str_builder_push_str(&bld, file_str);
    str_builder_null_terminate(&bld);
    const char *filepath = str_builder_compose(&bld, NULL).s;

    // LOGF("Reading api from: %s", filepath);

    FILE *f = fopen(filepath, "rb");

    parse_and_generate_tree(f, out, temp_arena);

    fclose(f);


    VD_RETURN_SCRATCH_ARENA(temp_arena);
}

static void process_paste(VdDspcSection *section, FILE *out, int depth)
{
    Arena *temp_arena = VD_GET_SCRATCH_ARENA();
    Str filepath = make_str_from_tag_value(vd_dspc_section_first_tag(section));

    StrBuilder bld;
    str_builder_init(&bld, temp_arena);
    str_builder_push_str(&bld, filepath);
    str_builder_null_terminate(&bld);
    const char *cstr_filepath = str_builder_compose(&bld, NULL).s;

    FILE *f = fopen(cstr_filepath, "r");
    char buf[1024];

    while (fgets(buf, 1024, f)) {
        PUT("%s", buf);
    }

    fclose(f);

    VD_RETURN_SCRATCH_ARENA(temp_arena);
}

static void process_api_remarks(VdDspcSection *section, FILE *out, int depth)
{
    PUT_LINE("<h6>Remarks</h6>");
    PUT_LINE("<p>");
    process_children(section, out, depth);
    PUT_LINE("</p>");
}

static void process_api_params(VdDspcSection *section, FILE *out, int depth)
{
    PUT_LINE("<table class=\"table\">");
    PUT_LINE("<tbody>");
    for (VdDspcSection *child = section->first; child; child = child->next) {
        Str param_name = make_str_from_tag_value(vd_dspc_section_first_tag(child));
        PUT_LINE("<tr>");
        PUT_LINE("<td>%.*s</td>", STR_EXPAND(param_name));
        PUT_LINE("<td>");
        process_children(child, out, depth);
        PUT_LINE("</td>");
        PUT_LINE("</tr>");
    }
    PUT_LINE("</tbody>");
    PUT_LINE("</table>");
}

static void process_api_brief(VdDspcSection *section, FILE *out, int depth)
{
    PUT_LINE("<p class=\"item-desc\">");
    process_children(section, out, depth);
    PUT_LINE("</p>");
}

static void process_api_details(VdDspcSection *section, FILE *out, int depth)
{
    PUT_LINE("<div class=\"apidetails\">");
    process_children(section, out, depth);
    PUT_LINE("</div>");
}

static void process_api_decl(VdDspcSection *section, FILE *out, int depth)
{
    Str decl_str = make_str_from_tag_value(vd_dspc_section_find_tag_with_name(section, NULL));
    PUT_LINE("<span class=\"item-decl\">%.*s</span>", STR_EXPAND(decl_str));
}

static void process_api(VdDspcSection *section, FILE *out, int depth)
{
    Str id_str = make_str_from_tag_value(vd_dspc_section_find_tag_with_name(section, NULL));

    PUT_LINE("<div id=\"%.*s\" class=\"L2 apiitem function\">", STR_EXPAND(id_str));

    process_children(section, out, depth);

    PUT_LINE("</div>");
}

static void process_rev(VdDspcSection *section, FILE *out, int depth)
{
    PUT_LINE("<div class=\"list-group\">");
    for (VdDspcSection *child = section->first; child; child = child->next) {
        Str version_str = make_str_from_tag_value(vd_dspc_section_find_tag_with_name(child, "version"));
        Str date_str = make_str_from_tag_value(vd_dspc_section_find_tag_with_name(child, "date"));
        Str avail_str = make_str_from_tag_value(vd_dspc_section_find_tag_with_name(child, "available"));

        if (str_eq(avail_str, LIT("no"))) {
            PUT_LINE("<a class=\"list-group-item revision-planned\">");
        } else {
            PUT_LINE("<a class=\"list-group-item\">");
        }
                PUT_LINE("<div class=\"d-flex w-100 justify-content-between\">");

                    PUT_LINE("<h5 class=\"mb-1\">%.*s</h5>", STR_EXPAND(version_str));
                    PUT_LINE("<small class=\"text-body-secondary\">%.*s</small>", STR_EXPAND(date_str));
                PUT_LINE("</div>");

                PUT_LINE("<p class=\"mb-1\">");
                    process_child(child->first, out, depth);
                PUT_LINE("</p>");
            PUT_LINE("</a>");
    }
    PUT_LINE("</div>");
}

static void process_tblock(VdDspcSection *section, FILE *out, int depth)
{
    PUT_LINE("<pre class=\"rounded copy\"><code class=\"language-txt\">");
    VdDspcStrNode *node = vd_dspc_str_list_first_node(&section->first->text_content);
    Str s = make_str_from_str_node(node);
    for (size_t i = 0; i < s.len; ++i) {
        char c = s.s[i];

        if (c == '\r') {
            continue;
        }
        PUT("%c", c);
    }
    PUT_LINE("</code></pre>");
}

static void process_carousel(VdDspcSection *section, FILE *out, int depth)
{
    VdDspcTag *carousel_tag = vd_dspc_section_first_tag(section);
    Str carousel_tag_id = make_str_from_tag_value(carousel_tag);

    PUT_LINE("<div id=\"%.*s\" class=\"rounded carousel slide\">", STR_EXPAND(carousel_tag_id));
        PUT_LINE("<div class=\"carousel-inner\">");
            int first = 0;
            for (VdDspcSection *child = section->first; child; child = child->next) {
                Str child_image_path = make_str_from_tag_value(vd_dspc_section_first_tag(child));

                if (!first) {
                    PUT_LINE("<div class=\"carousel-item active\">");
                    first = 1;
                } else {
                    PUT_LINE("<div class=\"carousel-item\">");
                }

                PUT_LINE("<img class=\"img-fluid\" src=\"%.*s\">", STR_EXPAND(child_image_path));

                PUT_LINE("</div>");
            }
        PUT_LINE("</div>");

        PUT_LINE("<button class=\"carousel-control-prev\" type=\"button\" data-bs-target=\"#%.*s\""
                 "        data-bs-slide=\"prev\">",
                 STR_EXPAND(carousel_tag_id));
            PUT_LINE("<span class=\"carousel-control-prev-icon\" style=\"filter: invert(1) grayscale(100%%) brightness(200%%);\""
                     "      aria-hidden=\"true\"></span>");
            PUT_LINE("<span class=\"visually-hidden\">Previous</span>");
        PUT_LINE("</button>");

        PUT_LINE("<button class=\"carousel-control-next\" type=\"button\" data-bs-target=\"#%.*s\""
                 "        data-bs-slide=\"next\">",
                 STR_EXPAND(carousel_tag_id));
            PUT_LINE("<span class=\"carousel-control-next-icon\" style=\"filter: invert(1) grayscale(100%%) brightness(200%%);\""
                     "      aria-hidden=\"true\"></span>");
            PUT_LINE("<span class=\"visually-hidden\">Next</span>");
        PUT_LINE("</button>");
    PUT_LINE("</div>");
}

static void process_copyright(VdDspcSection *section, FILE *out, int depth)
{
    PUT_LINE("<div class=\"d-flex\">");
        PUT_LINE("<span class=\"text-muted flex-grow-1 align-self-end\">&copy; Michael Dodis, 2025-2026.</span>");
        PUT_LINE("<span class=\"text-muted flex-grow-0 text-end align-self-end\">This page was created with DOCUSPEC</span>");
    PUT_LINE("</div>");
}

static void process_br(VdDspcSection *section, FILE *out, int depth)
{
    PUT_LINE("<br>");
}

static void process_accordion(VdDspcSection *section, FILE *out, int depth)
{
    VdDspcTag *tag_id = vd_dspc_section_find_tag_with_name(section, "id");
    Str tag_id_value = make_str_from_tag_value(tag_id);
    if (!tag_id) {
        ERR_EXIT("@accordion must have id!");
    }

    PUT_LINE("<div class=\"accordion\" id=\"%.*s\">", STR_EXPAND(tag_id_value));

    int child_count = 0;
    for (VdDspcSection *child = section->first; child; child = child->next) {
        Str child_id_str = make_str_from_section_id(child);
        
        if (!str_eq(child_id_str, LIT("accordion-item"))) {
            ERR_EXIT("Expected accordion-item under accordion directive!");
        }

        VdDspcTag *tag_title = vd_dspc_section_find_tag_with_name(child, NULL);
        Str tag_title_value = make_str_from_tag_value(tag_title);

        PUT_LINE("<div class=\"accordion-item\">");
        PUT_LINE("<p class=\"accordion-header\">");
        PUT_LINE("<button class=\"accordion-button\" type=\"button\""
                 "        data-bs-toggle=\"collapse\" data-bs-target=\"#%.*scollapse%d\""
                 "        aria-expanded=\"true\" aria-controls=\"%.*scollapse%d\">",
                 STR_EXPAND(tag_id_value), child_count,
                 STR_EXPAND(tag_id_value), child_count);
        PUT_LINE("%.*s", STR_EXPAND(tag_title_value));
        PUT_LINE("</button>");
        PUT_LINE("</p>");

        PUT_LINE("<div id=\"%.*scollapse%d\" class=\"accordion-collapse collapse\""
                 "     data-bs-parent=\"#%.*s\">",
                 STR_EXPAND(tag_id_value), child_count,
                 STR_EXPAND(tag_id_value));
        PUT_LINE("<div class=\"accordion-body\">");
        process_child(child->first, out, depth);
        PUT_LINE("</div>");
        PUT_LINE("</div>");

        PUT_LINE("</div>");

        child_count++;

    }

    PUT_LINE("</div>");
}

static void process_text(VdDspcSection *section, FILE *out, int depth)
{
    VdDspcStrNode *node = vd_dspc_str_list_first_node(&section->text_content);
    while (node) {

        if (node->flags & VD_DSPC_STR_NODE_FLAGS_ITALIC) {
            PUT("<i>");
        }

        if (node->flags & VD_DSPC_STR_NODE_FLAGS_BOLD) {
            PUT("<b>");
        }

        if (node->flags & VD_DSPC_STR_NODE_FLAGS_CODE) {
            PUT("<code>");
        }

        if (node->flags & VD_DSPC_STR_NODE_FLAGS_STRIKE) {
            PUT("<s>");
        }

        if (node->flags & VD_DSPC_STR_NODE_FLAGS_LINK) {
            PUT("<a href=\"%.*s\">", (int)node->link_str.l, node->link_str.s); 
        }

        Str s = make_str_from_str_node(node);
        PUT("%.*s", STR_EXPAND(s));

        if (node->flags & VD_DSPC_STR_NODE_FLAGS_SPACE) {
            PUT(" ");
        }

        if (node->flags & VD_DSPC_STR_NODE_FLAGS_LINK) {
            PUT("</a>"); 
        }

        if (node->flags & VD_DSPC_STR_NODE_FLAGS_STRIKE) {
            PUT("</s>");
        }

        if (node->flags & VD_DSPC_STR_NODE_FLAGS_CODE) {
            PUT("</code>");
        }

        if (node->flags & VD_DSPC_STR_NODE_FLAGS_BOLD) {
            PUT("</b>");
        }

        if (node->flags & VD_DSPC_STR_NODE_FLAGS_ITALIC) {
            PUT("</i>");
        }
        node = vd_dspc_str_list_next_node(node);
    }
}

typedef enum {
    LIST_MODE_ORD = 'o',
    LIST_MODE_UNORD = 'u',
    LIST_MODE_FANCY = 'f',
} ListMode;

static void process_list(VdDspcSection *section, FILE *out, int depth)
{
    VdDspcTag *mode = vd_dspc_section_first_tag(section);
    Str mode_str = make_str_from_tag_value(mode);

    ListMode list_mode = LIST_MODE_ORD;
    const char *tag = "ol";
    const char *klass = "";

    if (0) {
    } else if (str_eq(mode_str, LIT("o"))) {
        list_mode = LIST_MODE_ORD;
    } else if (str_eq(mode_str, LIT("u"))) {
        list_mode = LIST_MODE_UNORD;
        tag = "ul";
    } else if (str_eq(mode_str, LIT("f"))) {
        list_mode = LIST_MODE_FANCY;
        klass = "list-group";
    }

    PUT_LINE("<%s class=\"%s\">", tag, klass);

    process_children(section, out, depth);

    PUT_LINE("</%s>", tag);
}

static void process_item(VdDspcSection *section, FILE *out, int depth)
{
    VdDspcTag *mode = vd_dspc_section_first_tag(section->parent);
    Str mode_str = make_str_from_tag_value(mode);
    const char *klass = "";
    if (str_eq(mode_str, LIT("f"))) {
        klass = "list-group-item";
    }

    PUT("<li class=\"%s\">", klass);
    process_children(section, out, depth);
    PUT("</li>");
}

static void process_para(VdDspcSection *section, FILE *out, int depth)
{
    PUT_LINE("<p>");
    VdDspcStrNode *node = vd_dspc_str_list_first_node(&section->text_content);
    while (node) {

        if (node->flags & VD_DSPC_STR_NODE_FLAGS_ITALIC) {
            PUT("<i>");
        }

        if (node->flags & VD_DSPC_STR_NODE_FLAGS_BOLD) {
            PUT("<b>");
        }

        if (node->flags & VD_DSPC_STR_NODE_FLAGS_CODE) {
            PUT("<code>");
        }

        if (node->flags & VD_DSPC_STR_NODE_FLAGS_STRIKE) {
            PUT("<s>");
        }

        if (node->flags & VD_DSPC_STR_NODE_FLAGS_LINK) {
            PUT("<a href=\"%.*s\">", (int)node->link_str.l, node->link_str.s); 
        }

        Str s = make_str_from_str_node(node);
        PUT("%.*s", STR_EXPAND(s));

        if (node->flags & VD_DSPC_STR_NODE_FLAGS_SPACE) {
            PUT(" ");
        }

        if (node->flags & VD_DSPC_STR_NODE_FLAGS_LINK) {
            PUT("</a>"); 
        }

        if (node->flags & VD_DSPC_STR_NODE_FLAGS_STRIKE) {
            PUT("</s>");
        }

        if (node->flags & VD_DSPC_STR_NODE_FLAGS_CODE) {
            PUT("</code>");
        }

        if (node->flags & VD_DSPC_STR_NODE_FLAGS_BOLD) {
            PUT("</b>");
        }

        if (node->flags & VD_DSPC_STR_NODE_FLAGS_ITALIC) {
            PUT("</i>");
        }
        node = vd_dspc_str_list_next_node(node);
    }
    PUT("\n");   
    PUT_LINE("</p>");
}

static void process_code(VdDspcSection *section, FILE *out, int depth)
{
    PUT_LINE("<pre class=\"rounded copy\"><code class=\"language-cpp\">");
    VdDspcStrNode *node = vd_dspc_str_list_first_node(&section->text_content);
    Str s = make_str_from_str_node(node);
    for (size_t i = 0; i < s.len; ++i) {
        char c = s.s[i];

        if (c == '\r') {
            continue;
        }
        PUT("%c", c);
    }
    PUT_LINE("</code></pre>");
}

static void output_section_id(VdDspcSection *section, FILE *out)
{
    Str title = make_str_from_tag_value(vd_dspc_section_first_tag(section));
    for (int i = 0; i < title.len; ++i) {
        char c = title.s[i];
        if (is_upppercase(c)) {
            PUT("%c", uppercase_to_lowercase(c));
        } else if (c == ' ') {
            PUT("-");
        } else if (c == '&') {
            PUT("and");
        } else {
            PUT("%c", c);
        }
    }
}

static void process_section(VdDspcSection *section, FILE *out, int depth)
{
    Str title = make_str_from_tag_value(vd_dspc_section_first_tag(section));
    PUT("<section class=\"L%d section\" id=\"", depth);
    output_section_id(section, out);
    PUT("\">\n");
    PUT_LINE("<h4>%.*s</h4>", STR_EXPAND(title));
    if (depth == 1) {
        PUT_LINE("<hr>");
    }
    for (VdDspcSection *child = section->first; child; child = child->next) {
        process_child(child, out, depth + 1);
    }
    PUT_LINE("</section>");
    if (depth != 1) {
        PUT_LINE("<br>");
    }
}

static void parse_api_function(char *buf, size_t buf_len, FILE *f, FILE *out, Arena *temp_arena)
{
    ArenaSave save = arena_save(temp_arena);

    char *brief_part = 0;
    char *return_part = 0;
    dynarray const char **param_part = 0;
    dynarray_init_with_cap(param_part, temp_arena, 12);


    while (fgets(buf, buf_len, f)) {

        int i = 0;

        if (buf[i + 0] == ' ' &&
            buf[i + 1] == '*' &&
            buf[i + 2] == '/')
        {
            break;
        }

        // Skip whitespaces
        while ((i < buf_len) && (buf[i] == ' ' || buf[i] == '*') && (buf[i] != '\0')) {
            i++;
        }

        // LOGF("Buf: %s parse", buf + i);
        if (buf[i + 0] == '@' &&
            buf[i + 1] == 'b' &&
            buf[i + 2] == 'r' &&
            buf[i + 3] == 'i' &&
            buf[i + 4] == 'e' &&
            buf[i + 5] == 'f')
        {
            const char *s = buf + i + 6;
            brief_part = vd_cstr_dup(temp_arena, s);
            continue;
        }

        if (buf[i + 0] == '@' &&
            buf[i + 1] == 'p' &&
            buf[i + 2] == 'a' &&
            buf[i + 3] == 'r' &&
            buf[i + 4] == 'a' &&
            buf[i + 5] == 'm')
        {
            const char *s = buf + i + 6;
            const char *sd = vd_cstr_dup(temp_arena, s);

            dynarray_add(param_part, sd);
            continue;
        }

        if (buf[i + 0] == '@' &&
            buf[i + 1] == 'r' && 
            buf[i + 2] == 'e' && 
            buf[i + 3] == 't' && 
            buf[i + 4] == 'u' && 
            buf[i + 5] == 'r' && 
            buf[i + 6] == 'n')
        {

            const char *s = buf + i + 7;
            const char *sd = vd_cstr_dup(temp_arena, s);

            return_part = (char*)sd;
            continue;
        }

    }

    // Get the function decl
    assert(fgets(buf, buf_len, f));
    const char *func_decl = buf;

    Str func_decl_str = str_from_cstr((char*)func_decl);
    Str id_str = func_decl_str;

    // Get function id
    {
        // Find first parenthesis
        size_t paren_pos = str_first_of(func_decl_str, LIT("("), 0);
        size_t id_pos = paren_pos;

        // Walk backwards until we find a space
        while (id_pos > 0 && func_decl_str.s[id_pos] != ' ') {
            id_pos--;
        }

        id_str.s = func_decl_str.s + id_pos + 1;
        id_str.len = paren_pos - id_pos - 1;

    }
    // LOGF("%.*s", STR_EXPAND(id_str));

    Str func_decl_pretty = func_decl_str;
    // Get decl without _API, etc, i.e skip first word
    {
        uint64_t i = 0;
        uint64_t l = func_decl_str.len;

        while ((i < l) && (func_decl_str.s[i] != ' ')) {
            i++;
        }

        func_decl_pretty = str_chop_left(func_decl_str, i);

    }

    FILE *prevout = out;
    if (get_workspace()->search_index_file) {
        out = get_workspace()->search_index_file;
        int id = search_index_id();
        search_index_begin_entry(get_workspace()->search_index_file);


        PUT_LINE("\"id\": %d,", id);
        PUT_LINE("\"page\": \"%.*s.html\",", STR_EXPAND(get_workspace()->current_source_file->name));
        PUT("\"title\": ");
        put_json_str(out, display_title(get_workspace()->current_source_file));
        PUT(",\n");
        PUT_LINE("\"section\": \"%.*s\",", STR_EXPAND(id_str));
        PUT_LINE("\"subtitle\": \"%.*s\",", STR_EXPAND(id_str));
        PUT("\"contents\": \"");
        PUT("%.*s", STR_EXPAND(id_str));
        PUT("\"");


        search_index_end_entry(get_workspace()->search_index_file);
    }

    out = prevout;



    PUT_LINE("<div id=\"%.*s\" class=\"L2 apiitem function\">", STR_EXPAND(id_str));
        PUT_LINE("<span class=\"item-decl\">%.*s</span>", STR_EXPAND(func_decl_pretty));
        PUT_LINE("<div class=\"apidetails\">");
            PUT_LINE("<p class=\"item-desc\">");
            PUT_LINE("%s", brief_part);
            PUT_LINE("</p>");


            PUT_LINE("<table class=\"table\">");
            PUT_LINE("<tbody>");
            for (size_t pi = 0; pi < dynarray_len(param_part); ++pi) {
                const char *param = param_part[pi];

                Str param_name = str_null();
                Str param_desc = str_null();

                {
                    size_t l = vd_cstr_len(param);
                    size_t i = 0;

                    // Find name part
                    while ((i < l) && !is_letter(param[i])) {
                        i++;
                    }

                    size_t pos_param_name_start = i++;
                    while ((i < l) && (param[i] != ' ')) {
                        i++;
                    }
                    size_t pos_param_name_end = i;

                    while ((i < l) && !is_letter(param[i])) {
                        i++;
                    }
                    size_t pos_param_desc_start = i;

                    param_name.s = (char*)param + pos_param_name_start;
                    param_name.len = pos_param_name_end - pos_param_name_start;

                    param_desc.s = (char*)param + pos_param_desc_start;
                    param_desc.len = l - pos_param_desc_start;

                }

                PUT_LINE("<tr>");
                PUT_LINE("<td>%.*s</td>", STR_EXPAND(param_name));
                PUT_LINE("<td>");
                PUT_LINE("%.*s", STR_EXPAND(param_desc));
                PUT_LINE("</td>");
                PUT_LINE("</tr>");
            }

            if (return_part) {
                PUT_LINE("<tr>");
                PUT_LINE("<td>Returns</td>");
                PUT_LINE("<td>");
                PUT_LINE("%s", return_part);
                PUT_LINE("</td>");
                PUT_LINE("</tr>");
            }

            PUT_LINE("</tbody>");
            PUT_LINE("</table>");
        PUT_LINE("</div>");
    PUT_LINE("</div>");

    arena_restore(save);
}

static void parse_and_generate_tree(FILE *f, FILE *out, Arena *temp_arena)
{
    char buf[1024];

    while (fgets(buf, 1024, f)) {
        if (buf[0] == '/' &&
            buf[1] == '*' &&
            buf[2] == '*')
        {
            parse_api_function(buf, 1024, f, out, temp_arena);
        }
    }
}

// process_child already skipped this section if it doesn't match the build mode.
static void process_if_prototype(VdDspcSection *section, FILE *out, int depth)
{
    process_children(section, out, depth);
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* ----DOCUBUILD IMPLEMENTATION-------------------------------------------------------------------------------------- */
/* ------------------------------------------------------------------------------------------------------------------ */
static int Processor_Table_Length;

#define INVOC_EXIT() ERR_EXIT("Invocation: docubuild2 <Directory> -o <Output Directory> [-m webpage|document] [-t <Template Directory>] [-p]\n" \
                              "\t-o\tSet output directory.\n"                                                                                \
                              "\t-m\tBuild mode, webpage (default) or document.\n"                                                           \
                              "\t-t\tTemplate directory holding <mode>.html, defaults to <Directory>/templates.\n"                           \
                              "\t-p\tProduction build, enables {{#prod}} in the template.\n")

static void collect_source_file(File *file, void *userdata);
static void output_section_id(VdDspcSection *section, FILE *out);
static void search_and_add_indexed_sections(VdDspcSection *section, IndexedSection *dynarray*indexed_sections, int *index_id);
static void generate_indexed_section_index(VdDspcSection *section, SourceFile *source_file, FILE *out);
static void write_search_index_sections(void);
static Str  make_path(Arena *arena, Str dir, Str file, const char *extension);
static int  parse_prototype(Str s, Prototype *prototype);
static int  get_prototype_arg(VdDspcSection *section, Prototype *prototype);
static void report(SourceFile *source_file, int is_error, Str in_section, const char *fmt, ...);
static void read_metadata(VdDspcTree *tree, SourceFile *source_file);
static void check_section(VdDspcSection *section, SourceFile *source_file, int flag_webpage_only, Str in_section);
static Str  load_template(Arena *arena, Str path);
static void validate_template(void);
static void expand_template(size_t begin, size_t end, TemplateScope *scope, FILE *out);

static Workspace *Global_Workspace;

int main(int argc, char const *argv[])
{
    vd_init(NULL);
    Processor_Table_Length = ARRAY_COUNT(Processor_Table);

    if (argc < 2) {
        INVOC_EXIT();
    }

    const char *directory_to_open = 0;
    const char *directory_to_write_to = 0;
    const char *template_dir_path = 0;
    Prototype  mode = PROTOTYPE_WEBPAGE;
    int        is_prod = 0;

    Arg arg = arg_new(argc, (char**)argv);
    arg_skip_program_name(&arg);

    while (!arg_at_end(&arg)) {

        Str argname;
        Str argvalue_str;

        if (!arg_get_name(&arg, &argname)) {
            VD_ASSERT(arg_get_str(&arg, &argvalue_str));

            directory_to_open = argvalue_str.s;

            continue;
        }

        if (0) {
        } else if (str_eq(argname, LIT("o"))) {
            if (!arg_get_str(&arg, &argvalue_str)) {
                ERR_EXIT("Expected argument after -o!\n");
            }
            directory_to_write_to = argvalue_str.s;
        } else if (str_eq(argname, LIT("m"))) {
            if (!arg_get_str(&arg, &argvalue_str) || !parse_prototype(argvalue_str, &mode) || (mode == PROTOTYPE_ANY)) {
                ERR_EXIT("Expected webpage or document after -m!\n");
            }
        } else if (str_eq(argname, LIT("t"))) {
            if (!arg_get_str(&arg, &argvalue_str)) {
                ERR_EXIT("Expected argument after -t!\n");
            }
            template_dir_path = argvalue_str.s;
        } else if (str_eq(argname, LIT("p"))) {
            is_prod = 1;
        } else {
            ERR_EXIT("Unknown option -%.*s!\n", STR_EXPAND(argname));
        }
    }

    if (directory_to_open == 0) {
        INVOC_EXIT();
    }

    if (directory_to_write_to == 0) {
        ERR_EXIT("You need to specify write directory with -o!\n");
    }

    Workspace workspace = {
        .source_dir = vd_str_from_cstr((char*)directory_to_open),
        .global_arena = vd_arena_from_malloc(MEGABYTES(64)),
        .is_prod = is_prod,
        .mode = mode,
    };

    Global_Workspace = &workspace;

    Str output_dir = vd_str_from_cstr((char*)directory_to_write_to);

    Str template_dir  = template_dir_path ? vd_str_from_cstr((char*)template_dir_path)
                                          : make_path(&workspace.global_arena, workspace.source_dir, LIT("templates"), "");
    Str template_file = (mode == PROTOTYPE_DOCUMENT) ? LIT("document") : LIT("webpage");
    workspace.template_path = make_path(&workspace.global_arena, template_dir, template_file, ".html");

    workspace.template_str = load_template(&workspace.global_arena, workspace.template_path);
    validate_template();

    dynarray_init_with_cap(workspace.source_files, &workspace.global_arena, 8);
    vd_directory_walk_recursively(directory_to_open, collect_source_file, &workspace);

    vd_dspc_document_init(&workspace.document, NULL);

    // Open and parse all source files
    for (size_t i = 0; i < dynarray_len(workspace.source_files); ++i) {
        SourceFile *source_file = &workspace.source_files[i];

        Str name = source_file->file_path_relative_to_source_dir;
        name = str_chop_right(name, str_last_of(name, LIT("."), VD_STR_MAX) - 1);
        source_file->name = name;

        Str path_relative_to_cwd = make_path(&workspace.global_arena, workspace.source_dir,
                                             source_file->file_path_relative_to_source_dir, "");

        size_t len;
        uint8_t *file = vd_dump_file_to_bytes(&workspace.global_arena, path_relative_to_cwd.s, &len);
        VD_ASSERT(file != 0);

        {
            vd_dspc_document_add(&workspace.document, (char*)file, len, source_file);

            VdDspcError *err = vd_dspc_document_first_error(&workspace.document);
            while (vd_dspc_error_valid(err)) {
                char message[128];
                int message_size = vd_dspc_error_get_message(err, sizeof(message), message);

                fprintf(stderr, "%.*s:%zu:%zu %.*s\n",
                                STR_EXPAND(source_file->file_path_relative_to_source_dir),
                                err->line, err->column,
                                message_size, message);

                err = vd_dspc_error_next(err);
            }
        }
    }

    // Decide what gets built, and validate it all before writing anything
    for (VdDspcTree *tree = vd_dspc_document_first_tree(&workspace.document); tree; tree = vd_dspc_document_next_tree(&workspace.document, tree)) {
        SourceFile *source_file = (SourceFile*)tree->userdata;

        read_metadata(tree, source_file);

        source_file->build = (mode == PROTOTYPE_WEBPAGE) || (source_file->prototype != PROTOTYPE_WEBPAGE);
        if (!source_file->build) {
            printf("skipped %.*s: @prototype(\"webpage\")\n", STR_EXPAND(source_file->file_path_relative_to_source_dir));
            continue;
        }

        int flag_webpage_only = (source_file->prototype == PROTOTYPE_DOCUMENT) ||
                                ((source_file->prototype == PROTOTYPE_ANY) && (mode == PROTOTYPE_DOCUMENT));
        check_section(vd_dspc_tree_first_section(tree), source_file, flag_webpage_only, str_null());
    }

    if (workspace.error_count > 0) {
        ERR_EXIT("%d error(s), nothing was written.\n", workspace.error_count);
    }

    if (mode == PROTOTYPE_WEBPAGE) {
        Str output_file = make_path(&workspace.global_arena, output_dir, LIT("search-index"), ".json");

        workspace.search_index_file = fopen(output_file.s, "w");
        if (!workspace.search_index_file) {
            ERR_EXIT("Could not open %s for writing!\n", output_file.s);
        }
        fprintf(workspace.search_index_file, "[\n");

        workspace.search_index_id = 1;
    }

    // Generate html for all source files
    for (VdDspcTree *tree = vd_dspc_document_first_tree(&workspace.document); tree; tree = vd_dspc_document_next_tree(&workspace.document, tree)) {
        Arena *temp_arena = VD_GET_SCRATCH_ARENA();
        SourceFile *source_file = (SourceFile*)tree->userdata;

        if (!source_file->build) {
            VD_RETURN_SCRATCH_ARENA(temp_arena);
            continue;
        }

        // Distinct names, so a document build next to the site can't clobber it and still resolves its assets
        const char *extension = (mode == PROTOTYPE_DOCUMENT) ? ".document.html" : ".html";
        Str output_file = make_path(temp_arena, output_dir, source_file->name, extension);

        workspace.current_source_file = source_file;
        FILE *f = fopen(output_file.s, "w");
        if (!f) {
            ERR_EXIT("Could not open %s for writing!\n", output_file.s);
        }

        TemplateScope scope = {tree, source_file, NULL};
        expand_template(0, workspace.template_str.len, &scope, f);
        fclose(f);

        VD_RETURN_SCRATCH_ARENA(temp_arena);
    }

    if (workspace.search_index_file) {
        write_search_index_sections();
        fprintf(workspace.search_index_file, "]\n");
        fclose(workspace.search_index_file);
    }

    if (workspace.warning_count > 0) {
        fprintf(stderr, "%d warning(s).\n", workspace.warning_count);
    }

    return 0;
}

static Str make_path(Arena *arena, Str dir, Str file, const char *extension)
{
    StrBuilder bld;
    str_builder_init(&bld, arena);
    str_builder_push_str(&bld, dir);
    if ((dir.len > 0) && (dir.s[dir.len - 1] != '/') && (dir.s[dir.len - 1] != '\\')) {
        str_builder_push_str(&bld, LIT("/"));
    }
    str_builder_push_str(&bld, file);
    str_builder_push_cstr(&bld, extension);
    str_builder_null_terminate(&bld);
    return str_builder_compose(&bld, NULL);
}

/* ----PROTOTYPES---------------------------------------------------------------------------------------------------- */
static int parse_prototype(Str s, Prototype *prototype)
{
    if (0) {
    } else if (str_eq(s, LIT("any"))) {
        *prototype = PROTOTYPE_ANY;
    } else if (str_eq(s, LIT("webpage"))) {
        *prototype = PROTOTYPE_WEBPAGE;
    } else if (str_eq(s, LIT("document"))) {
        *prototype = PROTOTYPE_DOCUMENT;
    } else {
        return 0;
    }

    return 1;
}

static int get_prototype_arg(VdDspcSection *section, Prototype *prototype)
{
    VdDspcTag *tag = vd_dspc_section_first_tag(section);
    return tag && parse_prototype(make_str_from_tag_value(tag), prototype);
}

// Arguments are validated by check_section before anything renders.
static int if_prototype_matches(VdDspcSection *section)
{
    Prototype prototype = PROTOTYPE_ANY;
    get_prototype_arg(section, &prototype);
    return (prototype == PROTOTYPE_ANY) || (prototype == get_workspace()->mode);
}

// Shared by rendering and the search index, so the index only lists what was actually rendered.
static int is_rendered(VdDspcSection *section)
{
    Str section_id = make_str_from_section_id(section);
    if (str_eq(section_id, LIT("if_prototype"))) {
        return if_prototype_matches(section);
    }

    Processor *processor = find_processor(section_id);
    return !processor || (processor->prototype != PROTOTYPE_WEBPAGE) || (get_workspace()->mode == PROTOTYPE_WEBPAGE);
}

static void report(SourceFile *source_file, int is_error, Str in_section, const char *fmt, ...)
{
    Workspace *workspace = get_workspace();
    va_list args;

    fprintf(stderr, "%.*s: %s: ", STR_EXPAND(source_file->file_path_relative_to_source_dir), is_error ? "error" : "warning");
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    if (in_section.len != 0) {
        fprintf(stderr, " (in section \"%.*s\")", STR_EXPAND(in_section));
    }
    fprintf(stderr, "\n");

    if (is_error) {
        workspace->error_count++;
    } else {
        workspace->warning_count++;
    }
}

// YYYY-MM-DD, so dates sort as plain strings.
static int is_iso_date(Str s)
{
    if (s.len != 10) {
        return 0;
    }

    for (size_t i = 0; i < s.len; ++i) {
        int is_dash = (i == 4) || (i == 7);
        if (is_dash ? (s.s[i] != '-') : ((s.s[i] < '0') || (s.s[i] > '9'))) {
            return 0;
        }
    }

    int month = (s.s[5] - '0') * 10 + (s.s[6] - '0');
    int day   = (s.s[8] - '0') * 10 + (s.s[9] - '0');
    return (month >= 1) && (month <= 12) && (day >= 1) && (day <= 31);
}

static void read_metadata(VdDspcTree *tree, SourceFile *source_file)
{
    VdDspcSection *global_section = vd_dspc_tree_first_section(tree);
    for (VdDspcSection *child = global_section->first; child; child = child->next) {
        Str section_id = make_str_from_section_id(child);
        VdDspcTag *tag = vd_dspc_section_first_tag(child);

        if (0) {
        } else if (str_eq(section_id, LIT("title"))) {
            if (tag) source_file->title = make_str_from_tag_value(tag);
        } else if (str_eq(section_id, LIT("category"))) {
            if (tag) source_file->category = make_str_from_tag_value(tag);
        } else if (str_eq(section_id, LIT("date"))) {
            if (tag) source_file->date = make_str_from_tag_value(tag);
            if (!is_iso_date(source_file->date)) {
                report(source_file, 1, str_null(), "@date expects YYYY-MM-DD, got \"%.*s\"", STR_EXPAND(source_file->date));
            }
        } else if (str_eq(section_id, LIT("notoc"))) {
            source_file->notoc = VD_TRUE;
        } else if (str_eq(section_id, LIT("prototype"))) {
            if (!get_prototype_arg(child, &source_file->prototype)) {
                report(source_file, 1, str_null(), "@prototype expects \"webpage\", \"document\" or \"any\"");
            }
        }
    }
}

// Errors are reported for document files in either build mode, so a file that claims to be a document stays one.
static void check_section(VdDspcSection *section, SourceFile *source_file, int flag_webpage_only, Str in_section)
{
    for (VdDspcSection *child = section->first; child; child = child->next) {
        Str section_id = make_str_from_section_id(child);
        Str child_in_section = in_section;
        int child_flag_webpage_only = flag_webpage_only;

        if (str_eq(section_id, LIT("section"))) {
            VdDspcTag *tag = vd_dspc_section_first_tag(child);
            if (tag) child_in_section = make_str_from_tag_value(tag);
        }

        if (str_eq(section_id, LIT("if_prototype"))) {
            Prototype prototype;
            if (!get_prototype_arg(child, &prototype)) {
                report(source_file, 1, in_section, "@if_prototype expects \"webpage\", \"document\" or \"any\"");
                continue;
            }

            if (prototype == PROTOTYPE_WEBPAGE) {
                child_flag_webpage_only = 0;
            }
        } else if (flag_webpage_only) {
            Processor *processor = find_processor(section_id);
            if (processor && (processor->prototype == PROTOTYPE_WEBPAGE)) {
                if (source_file->prototype == PROTOTYPE_DOCUMENT) {
                    report(source_file, 1, in_section, "@%.*s is webpage-only, wrap it in @if_prototype(\"webpage\")",
                           STR_EXPAND(section_id));
                } else {
                    report(source_file, 0, in_section, "@%.*s is webpage-only, dropped from document build",
                           STR_EXPAND(section_id));
                }

                // Only report the outermost one, but keep validating @if_prototype arguments underneath
                child_flag_webpage_only = 0;
            }
        }

        check_section(child, source_file, child_flag_webpage_only, child_in_section);
    }
}

/* ----TEMPLATES----------------------------------------------------------------------------------------------------- */
// {{name}}                   inserts a value: content, title, name, category, date;
//                            doc_name, doc_title and doc_date inside docs.
// {{#name}} ... {{/name}}    renders once if name is set, or once per item for docs (every built page but index,
//                            newest @date first, undated pages last).
// {{^name}} ... {{/name}}    renders once if name is not set.
//                            Flags: toc (no @notoc), prod (-p), title, category, date, docs; doc_date inside docs.
// A section tag alone on its line takes the whole line with it, so they can be indented along with the html.
static SourceFile Template_Dry_Run_File;

static void template_error(size_t at, const char *fmt, ...)
{
    Workspace *workspace = get_workspace();
    va_list args;

    int line = 1;
    for (size_t i = 0; i < at; ++i) {
        if (workspace->template_str.s[i] == '\n') {
            line++;
        }
    }

    fprintf(stderr, "%.*s:%d: template error: ", STR_EXPAND(workspace->template_path), line);
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
    exit(-1);
}

static Str load_template(Arena *arena, Str path)
{
    const char *cpath = cstr_from_str(arena, path);
    FILE *f = fopen(cpath, "rb");
    if (!f) {
        ERR_EXIT("Could not open template %s!\n", cpath);
    }

    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *bytes = (char*)vd_arena_alloc(arena, size + 1);
    size = fread(bytes, 1, size, f);
    fclose(f);

    // Pages are written in text mode, which already turns \n into \r\n on windows
    size_t len = 0;
    for (size_t i = 0; i < size; ++i) {
        if (bytes[i] != '\r') {
            bytes[len++] = bytes[i];
        }
    }

    Str result = {bytes, len};
    return result;
}

static size_t template_find_pair(Str t, char c, size_t from, size_t end)
{
    for (size_t i = from; i + 1 < end; ++i) {
        if ((t.s[i] == c) && (t.s[i + 1] == c)) {
            return i;
        }
    }
    return end;
}

static int template_next_tag(Str t, size_t from, size_t end, TemplateTag *tag)
{
    size_t open = template_find_pair(t, '{', from, end);
    if (open == end) {
        return 0;
    }

    size_t close = template_find_pair(t, '}', open + 2, end);
    if (close == end) {
        template_error(open, "{{ is never closed");
    }

    Str name = {t.s + open + 2, close - open - 2};
    while ((name.len > 0) && (name.s[0] == ' ')) {
        name.s++;
        name.len--;
    }
    while ((name.len > 0) && (name.s[name.len - 1] == ' ')) {
        name.len--;
    }

    tag->kind = 0;
    if ((name.len > 0) && ((name.s[0] == '#') || (name.s[0] == '^') || (name.s[0] == '/'))) {
        tag->kind = name.s[0];
        name.s++;
        name.len--;
    }

    tag->open = open;
    tag->close = close + 2;
    tag->name = name;
    return 1;
}

static void template_line_extent(Str t, TemplateTag *tag, size_t *line_begin, size_t *line_end)
{
    *line_begin = tag->open;
    *line_end = tag->close;
    if (tag->kind == 0) {
        return;
    }

    size_t b = tag->open;
    while ((b > 0) && ((t.s[b - 1] == ' ') || (t.s[b - 1] == '\t'))) {
        b--;
    }
    if ((b > 0) && (t.s[b - 1] != '\n')) {
        return;
    }

    size_t e = tag->close;
    while ((e < t.len) && ((t.s[e] == ' ') || (t.s[e] == '\t'))) {
        e++;
    }
    if ((e < t.len) && (t.s[e] != '\n')) {
        return;
    }
    if (e < t.len) {
        e++;
    }

    *line_begin = b;
    *line_end = e;
}

static void template_find_close(Str t, TemplateTag *open_tag, size_t from, size_t end, TemplateTag *close_tag)
{
    int depth = 1;
    while (template_next_tag(t, from, end, close_tag)) {
        from = close_tag->close;

        if (!str_eq(close_tag->name, open_tag->name)) {
            continue;
        }

        if ((close_tag->kind == '#') || (close_tag->kind == '^')) {
            depth++;
        } else if ((close_tag->kind == '/') && (--depth == 0)) {
            return;
        }
    }

    template_error(open_tag->open, "{{%c%.*s}} is never closed", open_tag->kind, STR_EXPAND(open_tag->name));
}

static void template_write(Str t, size_t from, size_t to, FILE *out)
{
    if (out && (to > from)) {
        fwrite(t.s + from, 1, to - from, out);
    }
}

static int is_listed_doc(SourceFile *source_file)
{
    return source_file->build && !str_eq(source_file->name, LIT("index"));
}

static int compare_str(Str a, Str b)
{
    size_t len = (a.len < b.len) ? a.len : b.len;
    int result = (len > 0) ? memcmp(a.s, b.s, len) : 0;
    if (result == 0) {
        result = (a.len < b.len) ? -1 : (a.len > b.len) ? 1 : 0;
    }
    return result;
}

// Newest first, undated last, then by name so the order doesn't depend on the directory walk.
static int compare_listed_docs(const void *pa, const void *pb)
{
    SourceFile *a = *(SourceFile *const *)pa;
    SourceFile *b = *(SourceFile *const *)pb;

    if ((a->date.len == 0) != (b->date.len == 0)) {
        return (a->date.len == 0) ? 1 : -1;
    }

    int result = compare_str(b->date, a->date);
    return (result != 0) ? result : compare_str(a->name, b->name);
}

static void template_require_doc(TemplateTag *tag, TemplateScope *scope)
{
    if (!scope->doc) {
        template_error(tag->open, "{{%.*s}} is only valid inside {{#docs}}", STR_EXPAND(tag->name));
    }
}

static void expand_section(TemplateTag *tag, size_t begin, size_t end, TemplateScope *scope, FILE *out)
{
    Workspace *workspace = get_workspace();
    SourceFile *source_file = scope->source_file;
    int inverted = (tag->kind == '^');
    int is_set = 0;

    if (str_eq(tag->name, LIT("docs"))) {
        SourceFile *prev_doc = scope->doc;

        // Validation: run the body once with a placeholder item
        if (!out) {
            scope->doc = inverted ? prev_doc : &Template_Dry_Run_File;
            expand_template(begin, end, scope, out);
            scope->doc = prev_doc;
            return;
        }

        Arena *temp_arena = VD_GET_SCRATCH_ARENA();
        dynarray SourceFile **docs = 0;
        dynarray_init(docs, temp_arena);

        for (VdDspcTree *tree = vd_dspc_document_first_tree(&workspace->document); tree; tree = vd_dspc_document_next_tree(&workspace->document, tree)) {
            SourceFile *doc = (SourceFile*)tree->userdata;
            if (is_listed_doc(doc)) {
                dynarray_add(docs, doc);
            }
        }

        size_t doc_count = dynarray_len(docs);
        if (doc_count > 1) {
            qsort(docs, doc_count, sizeof(docs[0]), compare_listed_docs);
        }

        if (inverted) {
            if (doc_count == 0) {
                expand_template(begin, end, scope, out);
            }
        } else {
            for (size_t i = 0; i < doc_count; ++i) {
                scope->doc = docs[i];
                expand_template(begin, end, scope, out);
            }
            scope->doc = prev_doc;
        }

        VD_RETURN_SCRATCH_ARENA(temp_arena);
        return;
    }

    if (0) {
    } else if (str_eq(tag->name, LIT("toc"))) {
        is_set = !source_file->notoc;
    } else if (str_eq(tag->name, LIT("prod"))) {
        is_set = workspace->is_prod;
    } else if (str_eq(tag->name, LIT("title"))) {
        is_set = source_file->title.len != 0;
    } else if (str_eq(tag->name, LIT("category"))) {
        is_set = source_file->category.len != 0;
    } else if (str_eq(tag->name, LIT("date"))) {
        is_set = source_file->date.len != 0;
    } else if (str_eq(tag->name, LIT("doc_date"))) {
        template_require_doc(tag, scope);
        is_set = scope->doc->date.len != 0;
    } else {
        template_error(tag->open, "unknown section {{%c%.*s}}", tag->kind, STR_EXPAND(tag->name));
    }

    if (!out || (is_set != inverted)) {
        expand_template(begin, end, scope, out);
    }
}

static void expand_variable(TemplateTag *tag, TemplateScope *scope, FILE *out)
{
    SourceFile *source_file = scope->source_file;
    Str value = str_null();

    if (0) {
    } else if (str_eq(tag->name, LIT("content"))) {
        if (out) {
            // @title, @category etc. have no processor, so only content is output
            VdDspcSection *global_section = vd_dspc_tree_first_section(scope->tree);
            for (VdDspcSection *child = global_section->first; child; child = child->next) {
                process_child(child, out, 1);
            }
        }
        return;
    } else if (str_eq(tag->name, LIT("title"))) {
        value = source_file->title;
    } else if (str_eq(tag->name, LIT("name"))) {
        value = source_file->name;
    } else if (str_eq(tag->name, LIT("category"))) {
        value = source_file->category;
    } else if (str_eq(tag->name, LIT("date"))) {
        value = source_file->date;
    } else if (str_eq(tag->name, LIT("doc_name"))) {
        template_require_doc(tag, scope);
        value = scope->doc->name;
    } else if (str_eq(tag->name, LIT("doc_title"))) {
        template_require_doc(tag, scope);
        value = scope->doc->title;
    } else if (str_eq(tag->name, LIT("doc_date"))) {
        template_require_doc(tag, scope);
        value = scope->doc->date;
    } else {
        template_error(tag->open, "unknown variable {{%.*s}}", STR_EXPAND(tag->name));
    }

    if (out) {
        PUT("%.*s", STR_EXPAND(value));
    }
}

// out == NULL only validates, visiting every section body regardless of its flag.
static void expand_template(size_t begin, size_t end, TemplateScope *scope, FILE *out)
{
    Str t = get_workspace()->template_str;
    size_t at = begin;
    TemplateTag tag;

    while (template_next_tag(t, at, end, &tag)) {
        size_t line_begin, line_end;
        template_line_extent(t, &tag, &line_begin, &line_end);
        template_write(t, at, line_begin, out);
        at = line_end;

        switch (tag.kind) {
            case '#':
            case '^': {
                TemplateTag close_tag;
                size_t body_begin = at;
                size_t body_end, after;

                template_find_close(t, &tag, at, end, &close_tag);
                template_line_extent(t, &close_tag, &body_end, &after);
                expand_section(&tag, body_begin, body_end, scope, out);
                at = after;
            } break;

            case '/': template_error(tag.open, "{{/%.*s}} was never opened", STR_EXPAND(tag.name)); break;
            default: expand_variable(&tag, scope, out); break;
        }
    }

    template_write(t, at, end, out);
}

static void validate_template(void)
{
    TemplateScope scope = {NULL, &Template_Dry_Run_File, NULL};
    expand_template(0, get_workspace()->template_str.len, &scope, NULL);
}

/* ----SEARCH INDEX-------------------------------------------------------------------------------------------------- */
static void write_search_index_sections(void)
{
    Workspace *workspace = get_workspace();
    FILE *out = workspace->search_index_file;
    Arena *temp_arena = VD_GET_SCRATCH_ARENA();

    for (VdDspcTree *tree = vd_dspc_document_first_tree(&workspace->document); tree; tree = vd_dspc_document_next_tree(&workspace->document, tree)) {
        SourceFile *source_file = (SourceFile*)tree->userdata;

        if (!is_listed_doc(source_file)) {
            continue;
        }

        dynarray IndexedSection *indexed_sections = 0;
        dynarray_init(indexed_sections, temp_arena);

        search_and_add_indexed_sections(vd_dspc_tree_first_section(tree), &indexed_sections, &workspace->search_index_id);

        for (size_t i = 0; i < dynarray_len(indexed_sections); ++i) {
            IndexedSection *indexed_section = &indexed_sections[i];

            search_index_begin_entry(out);

            PUT_LINE("\"id\": %d,", indexed_section->id);
            PUT_LINE("\"page\": \"%.*s.html\",", STR_EXPAND(source_file->name));
            PUT("\"title\": ");
            put_json_str(out, display_title(source_file));
            PUT(",\n");
            PUT("\"section\": \"");
            output_section_id(indexed_section->section, out);
            PUT("\",\n");

            PUT("\"subtitle\": ");
            put_json_str(out, make_str_from_tag_value(vd_dspc_section_first_tag(indexed_section->section)));
            PUT(",\n");

            PUT("\"contents\": \"");
            for (VdDspcSection *child = indexed_section->section->first; child; child = child->next) {
                generate_indexed_section_index(child, source_file, out);
            }

            PUT("\"");
            search_index_end_entry(out);
        }
    }

    VD_RETURN_SCRATCH_ARENA(temp_arena);
}

static void generate_indexed_section_index(VdDspcSection *section, SourceFile *source_file, FILE *out)
{
    Str section_id = make_str_from_section_id(section);
    if (str_eq(section_id, LIT("section")) || !is_rendered(section)) {
        return;
    }

    for (VdDspcStrNode *node = vd_dspc_str_list_first_node(&section->text_content); node; node = vd_dspc_str_list_next_node(node)) {
        Str text = make_str_from_str_node(node);

        for (size_t i = 0; i < text.len; ++i) {
            char c = text.s[i];
            int c_is_whitespace =
                (c == '\t') ||
                (c == '\r') ||
                (c == '\n') ||
                (c == '\b');

            if (c_is_whitespace) {
                PUT(" ");
            } else {
                if (c == '\'' || c == '\"' || c == '\\') {
                    continue;
                }
                PUT("%c", text.s[i]);
            }
        }

        if (node->flags & VD_DSPC_STR_NODE_FLAGS_SPACE) {
            PUT(" ");
        }
    }

    for (VdDspcSection *child = section->first; child; child = child->next) {
        generate_indexed_section_index(child, source_file, out);
    }
}

static void search_and_add_indexed_sections(VdDspcSection *section, IndexedSection *dynarray*indexed_sections, int *index_id)
{
    if (!is_rendered(section)) {
        return;
    }

    Str section_id = make_str_from_section_id(section);
    if (str_eq(section_id, LIT("section"))) {
        IndexedSection new_indexed_section = {
            .section = section,
            .id = (*index_id)++,
        };
        dynarray_add(*indexed_sections, new_indexed_section);
    }

    for (VdDspcSection *child = section->first; child; child = child->next) {
        search_and_add_indexed_sections(child, indexed_sections, index_id);
    }
}

static void process_verbatim_html(VdDspcSection *section, FILE *out, int depth)
{
    Str section_id = make_str_from_section_id(section);

    PUT("<%.*s ", STR_EXPAND(section_id));

    for (VdDspcTag *tag = vd_dspc_section_first_tag(section); tag; tag = vd_dspc_section_next_tag(section, tag)) {
        Str tag_name = make_str_from_tag_name(tag);
        Str tag_value = make_str_from_tag_value(tag);
        PUT("%.*s=\"%.*s\" ", STR_EXPAND(tag_name), STR_EXPAND(tag_value));
    }
    PUT(">\n");

    for (VdDspcSection *child = section->first; child; child = child->next) {
        process_child(child, out, depth);
    }

    PUT("</%.*s>\n", STR_EXPAND(section_id));
}

static void process_children(VdDspcSection *section, FILE *out, int depth)
{
    for (VdDspcSection *child = section->first; child; child = child->next) {
        process_child(child, out, depth);
    }
}

static Processor *find_processor(Str section_id)
{
    for (int i = 0; i < Processor_Table_Length; ++i) {
        if (str_eq(section_id, Processor_Table[i].id)) {
            return &Processor_Table[i];
        }
    }
    return 0;
}

static void process_child(VdDspcSection *section, FILE *out, int depth)
{
    Processor *processor = find_processor(make_str_from_section_id(section));
    if (processor && is_rendered(section)) {
        processor->process(section, out, depth);
    }
}

static Workspace *get_workspace(void)
{
    return Global_Workspace;
}

static void collect_source_file(File *file, void *userdata)
{
    size_t dot = str_last_of(file->name, LIT("."), VD_STR_MAX);
    if (dot >= file->name.len) {
        return;
    }

    Str extension = str_chop_left(file->name, dot);
    if (!str_eq(extension, LIT("docuspec"))) {
        return;
    }

    Workspace *workspace = (Workspace*)userdata;
    SourceFile new_source_file = {
        .file_path_relative_to_source_dir = str_dup(&workspace->global_arena, file->name),
        .name = str_null(),
        .title = str_null(),
        .category = str_null(),
        .date = str_null(),
        .notoc = VD_FALSE,
        .prototype = PROTOTYPE_ANY,
        .build = VD_FALSE,
    };
    dynarray_add(workspace->source_files, new_source_file);
}

static Str make_str_from_section_id(VdDspcSection *section)
{
    Str result = {(char*)section->section_id.s, section->section_id.l};   
    return result;
}

static Str make_str_from_tag_name(VdDspcTag *tag)
{
    Str result = {(char*)tag->name.s, tag->name.l};
    return result;
}

static Str make_str_from_tag_value(VdDspcTag *tag)
{
    Str result = {(char*)tag->value.s, tag->value.l};
    return result;
}

static Str make_str_from_str_node(VdDspcStrNode *node)
{
    Str result = {(char*)node->str, node->len};
    return result;
}

static int search_index_id(void)
{
    return get_workspace()->search_index_id++;
}

static int search_index_curr_id(void)
{
    return get_workspace()->search_index_id - 1;
}

// Ids are handed out separately (sections get theirs while being collected), so count entries for the comma.
static void search_index_begin_entry(FILE *out)
{
    if (get_workspace()->search_index_entry_count++ == 0) {
        PUT_LINE("{");
    } else {
        PUT_LINE(",{");
    }
}

static void put_json_str(FILE *out, Str s)
{
    PUT("\"");
    for (size_t i = 0; i < s.len; ++i) {
        char c = s.s[i];
        if (0) {
        } else if ((c == '"') || (c == '\\')) {
            PUT("\\%c", c);
        } else if ((unsigned char)c < 0x20) {
            PUT(" ");
        } else {
            PUT("%c", c);
        }
    }
    PUT("\"");
}

static Str display_title(SourceFile *source_file)
{
    return (source_file->title.len != 0) ? source_file->title : source_file->name;
}

static void search_index_end_entry(FILE *out)
{
    PUT_LINE("}");
}

#define VD_DSPC_IMPL
#include "vd_docuspec.h"

#define VD_IMPL
#include "vd.h"
