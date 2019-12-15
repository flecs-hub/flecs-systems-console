#include <flecs_systems_console.h>
#include <flecs/util/dbg.h>

typedef struct ui_thread_t {
    ecs_world_t *world;
    ecs_entity_t console_entity;
    ecs_os_mutex_t mutex;
    ecs_snapshot_t *snapshot;
} ui_thread_t;

typedef struct ConsoleUiThread {
    ecs_os_thread_t thread;
    ui_thread_t *ctx;
} ConsoleUiThread;

static
void show_prompt(void) {
    printf("\nflecs$ ");
}

static
void print_column(
    const char *fmt,
    size_t len,
    ...)
{
    char buff[1024];
    va_list args;
    va_start(args, len);

    vsnprintf(buff, 1024, fmt, args);

    va_end(args);

    if (len) {
        printf("%s%*s", buff, (int)(len - strlen(buff)), "");
    } else {
        printf("%s\n", buff);
    }
}

static
void print_line(
    uint32_t len)
{
    int i;
    for (i = 0; i < len; i ++) {
        printf("-");
    }
    printf("\n");
}

static
char* read_cmd(
    FILE *file) 
{
    int len = 0, max = 32;
    char *result = ecs_os_malloc(max + 1);

    for(;;) {
        char ch = fgetc(file);
        if(ch == EOF)
            break;

        if (ch == '\n') 
            break;

        if (len >= max) {
            max *= 2;
            result = ecs_os_realloc(result, max + 1);
        }

        result[len ++] = ch;
    }

    if (result) {
        result[len] = '\0';
    }

    return result;  
}

static
const char* parse_arg(
    const char *args,
    char *arg)
{
    char *bptr = arg, ch;
    const char *ptr = args;

    while ((ch = *ptr) && ch && !isspace(ch)) {
        *bptr = ch;
        bptr ++;
        ptr ++;
    }

    if (!ch) {
        return NULL;
    }

    *bptr = '\0';

    return ptr;   
}

static
const char* is_cmd(
    const char *cmd, 
    const char *str) 
{
    size_t len = strlen(str);

    if (!strncmp(cmd, str, len)) {
        if (cmd[len]) {
            return cmd + len + 1;
        }  else {
            return cmd + len;
        }
    } else if (cmd[0] == str[0] && (!cmd[1] || isspace(cmd[1]))) {
        if (cmd[1]) {
            return cmd + 1 + 1;
        } else {
            return cmd + 1;
        }
    }

    return NULL;
}

static
void print_entity_header(void)
{
    printf("\n");
    print_column("id", 6);
    print_column("name", 20);
    print_column("type", 0);
    print_line(6 + 20 + strlen("type"));
}

static
void print_entity_summary(
    ecs_world_t *world,
    ecs_entity_t entity,
    ecs_type_t type)
{
    char *type_expr = NULL;

    if (type) {
        type_expr = ecs_type_to_expr(world, type);
    }

    const char *name = ecs_get_id(world, entity);

    print_column("%lld", 6, entity == ECS_SINGLETON ? 0 : entity);
    print_column("%s", 20, name ? name : "");
    print_column("[%s]", 0, type_expr ? type_expr : "");

    ecs_os_free(type_expr);
}

static
int dump_entities(
    ecs_world_t *world,
    ecs_type_filter_t *filter) 
{
    print_entity_header();

    ecs_table_t *table;
    int i = 0;
    while ((table = ecs_dbg_get_table(world, i ++))) {
        if (filter) {
            if (!ecs_dbg_filter_table(world, table, filter)) {
                continue;
            }
        }

        ecs_dbg_table_t dbg;
        ecs_dbg_table(world, table, &dbg);

        int e;
        for (e = 0; e < dbg.entities_count; e++) {
             print_entity_summary(world, dbg.entities[e], dbg.type);
        }
    }

    return 0;
}

static
ecs_entity_t parse_entity_id(
    ecs_world_t *world, 
    const char *id) 
{
    if (isdigit(id[0])) {
        return atoi(id);
    } else {
        return ecs_lookup(world, id);
    }
}

static
bool print_matched_with(
    ecs_world_t *world,
    ecs_dbg_table_t *table_dbg)
{
    if (table_dbg->systems_matched) {
        ecs_entity_t *systems = ecs_vector_first(table_dbg->systems_matched);
        uint32_t i, count = ecs_vector_count(table_dbg->systems_matched);
        for (i = 0; i < count; i ++) {
            ecs_entity_t system = systems[i];
            
            if (i) {
                printf(",");
            }
            
            printf("%s", ecs_get_id(world, system));
        }

        return true;
    } else {
        return false;
    }
}

static
void print_type_details(
    ecs_world_t *world,
    ecs_dbg_table_t *dbg_table,
    uint32_t column_width)
{
    print_column("type (shared):", column_width);
    if (dbg_table->shared) {
        char *type_expr = ecs_type_to_expr(world, dbg_table->shared);
        printf("[%s]\n", type_expr);
        free(type_expr);
    } else {
        printf("-\n");
    }

    print_column("type (container):", column_width);
    if (dbg_table->container) {
        char *type_expr = ecs_type_to_expr(world, dbg_table->container);
        printf("[%s]\n", type_expr);
        free(type_expr);
    } else {
        printf("-\n");
    }

    print_column("child of:", column_width);
    if (dbg_table->parent_entities) {
        char *type_expr = ecs_type_to_expr(world, dbg_table->parent_entities);
        printf("%s\n", type_expr);
        free(type_expr);        
    } else {
        printf("-\n");
    }

    print_column("inherits from:", column_width);
    if (dbg_table->base_entities) {
        char *type_expr = ecs_type_to_expr(world, dbg_table->base_entities);
        printf("%s\n", type_expr);
        free(type_expr);        
    } else {
        printf("-\n");
    }
}

static
int dump_entity(
    ecs_world_t *world, 
    ecs_entity_t e) 
{  
    int column_width = 24;
    char *type_expr;

    ecs_dbg_entity_t dbg;
    ecs_dbg_entity(world, e, &dbg);

    ecs_dbg_table_t dbg_table = {0};
    if (dbg.table) {
        ecs_dbg_table(world, dbg.table, &dbg_table);
    }

    print_column("id:", column_width);
    printf("%lld\n", e);

    const char *name = ecs_get_id(world, e);
    if (name) {
        print_column("name:", column_width);
        printf("%s\n", name);
    }

    type_expr = ecs_type_to_expr(world, dbg.type);
    print_column("type (owned):", column_width);
    printf("[%s]\n", type_expr);
    free(type_expr);

    print_type_details(world, &dbg_table, column_width);

    print_column("matched with:", column_width);
    if (!print_matched_with(world, &dbg_table)) {
        printf("-");
    }
    printf("\n");    

    print_column("is watched:", column_width);
    printf("%s\n", dbg.is_watched ? "true" : "false");

    print_column("row:", column_width);
    printf("%d\n", dbg.row);

    return 0;
}

static
int parse_type_filter(
    ecs_world_t *world,
    const char *args,
    ecs_type_filter_t *filter_out) 
{
    char *expr = ecs_os_strdup(args + 1);
    int len = strlen(expr);
    expr[len - 1] = '\0';
    filter_out->include = ecs_expr_to_type(world, expr);
    ecs_os_free(expr);
    return 0;
}

static
int cmd_entity(
    ecs_world_t *world, 
    const char *args) 
{
    if (!args[0]) {
        return dump_entities(world, NULL);
    } else if (args[0] == '[') {
        ecs_type_filter_t filter = {0};

        if (parse_type_filter(world, args, &filter)) {
            return -1;
        }

        return dump_entities(world, &filter);
    } else {
        ecs_entity_t e = parse_entity_id(world, args);
        if (!e) {
            return -1;
        }

        return dump_entity(world, e);
    }

    return 0;
}

static
void print_table_summary(
    ecs_world_t *world,
    ecs_table_t *table)
{
    ecs_dbg_table_t dbg;
    ecs_dbg_table(world, table, &dbg);

    char *type_expr = NULL;
    if (dbg.type) {
        type_expr = ecs_type_to_expr(world, dbg.type);
    }

    print_column("[%s]", 64, type_expr);
    print_column("%d", 12, dbg.entities_count);

    ecs_os_free(type_expr);  

    if (!print_matched_with(world, &dbg)) {
        printf("-");
    }

    printf("\n");
}

static
int dump_tables(
    ecs_world_t *world,
    ecs_type_filter_t *filter) 
{
    printf("\n");
    print_column("id", 4);
    print_column("type", 64);
    print_column("entities", 12);
    print_column("matched with", 0);
    print_line(4 + 48 + 16 + strlen("matched with"));

    ecs_table_t *table;
    int i = 0;
    while ((table = ecs_dbg_get_table(world, i ++))) {
        if (filter) {
            if (!ecs_dbg_filter_table(world, table, filter)) {
                continue;
            }
        }

        print_column("%d", 4, i);
        print_table_summary(world, table);
    }

    return 0;
}

static
int dump_table(
    ecs_world_t *world,
    uint32_t id)
{
    uint32_t column_width = 24;

    ecs_table_t *table = ecs_dbg_get_table(world, id - 1);
    if (!table) {
        return -1;
    }

    ecs_dbg_table_t dbg;
    ecs_dbg_table(world, table, &dbg);

    char *type_expr = ecs_type_to_expr(world, dbg.type);
    print_column("type (owned):", column_width);
    printf("[%s]\n", type_expr);
    ecs_os_free(type_expr);

    print_type_details(world, &dbg, column_width);

    print_column("entities:", column_width);
    printf("%d\n", dbg.entities_count);

    print_column("matched with:", column_width);
    if (!print_matched_with(world, &dbg)) {
        printf("-");
    }
    printf("\n");

    return 0;
}

static
int cmd_table(
    ecs_world_t *world, 
    const char *args) 
{
    if (!args[0]) {
        return dump_tables(world, NULL);
    } else if (args[0] == '[') {
        ecs_type_filter_t filter = {0};

        if (parse_type_filter(world, args, &filter)) {
            return -1;
        }

        return dump_tables(world, &filter);
    } else {
        if (isdigit(args[0])) {
            int id = atoi(args);
            dump_table(world, id);
        } else {
            return -1;
        }
    }

    return 0;
}

static
int print_system_summary(
    ecs_world_t *world,
    ecs_entity_t system)
{
    ecs_dbg_col_system_t dbg;
    if (ecs_dbg_col_system(world, system, &dbg)) {
        return -1;
    }
    
    print_column("%lld", 4, system);
    print_column("%s", 20, ecs_get_id(world, system));
    print_column("%d", 18, dbg.active_table_count + dbg.inactive_table_count);
    print_column("%d", 0, dbg.entities_matched_count);
    
    return 0;
}

static
int dump_system(
    ecs_world_t *world,
    ecs_entity_t system)
{
    uint32_t column_width = 32;
    ecs_dbg_col_system_t dbg;
    if (ecs_dbg_col_system(world, system, &dbg)) {
        return -1;
    }

    print_column("id:", column_width);
    printf("%lld\n", system);

    print_column("name:", column_width);
    printf("%s\n", ecs_get_id(world, system));

    print_column("enabled:", column_width);
    printf("%s\n", dbg.enabled ? "true" : "false");

    print_column("entities matched:", column_width);
    printf("%d\n", dbg.entities_matched_count);

    print_column("active matched:", column_width);
    printf("%d\n", dbg.active_table_count);

    print_column("inactive matched:", column_width);
    printf("%d\n", dbg.inactive_table_count);

    return 0;
}

static
int dump_systems(
    ecs_world_t *world) 
{
    ecs_type_filter_t filter = {
        .include = ecs_type(EcsColSystem)
    };

    printf("\n");
    print_column("id", 4);
    print_column("name", 20);
    print_column("tables matched", 18);
    print_column("entities matched", 0);
    print_line(4 + 20 + 12 + strlen("entities matched"));

    ecs_table_t *table;
    int i = 0;
    while ((table = ecs_dbg_get_table(world, i ++))) {
        if (!ecs_dbg_filter_table(world, table, &filter)) {
            continue;
        }

        ecs_dbg_table_t dbg;
        ecs_dbg_table(world, table, &dbg);

        int e;
        for (e = 0; e < dbg.entities_count; e++) {
            print_system_summary(world, dbg.entities[e]);
        }
    }

    return 0;
}

static
int cmd_system(
    ecs_world_t *world,
    const char *args)
{
    if (!args[0]) {
        return dump_systems(world);
    } else {
        ecs_entity_t e = parse_entity_id(world, args);
        if (!e) {
            return -1;
        }

        dump_system(world, e);
    }

    return 0;
}

static
int cmd_match(
    ecs_world_t *world,
    const char *args)
{
    char arg[256];
    const char *ptr = parse_arg(args, arg);
    if (!ptr) {
        return -1;
    }

    /* Skip whitespace */
    ptr ++;

    ecs_entity_t e = parse_entity_id(world, arg);
    if (!e) {
        return -1;
    }     

    ecs_entity_t system = parse_entity_id(world, ptr);
    if (!system) {
        return -1;
    }

    ecs_dbg_match_failure_t failure_info = {0};
    if (ecs_dbg_match_entity(world, e, system, &failure_info)) {
        printf("entitiy '%s' matches with system '%s'\n", 
            arg, ecs_get_id(world, system));
    } else {
        printf("entity '%s' does not match with system '%s'\n", 
            arg, ecs_get_id(world, system));

        ecs_type_t type = NULL;
        char *type_expr = NULL;

        if (failure_info.column) {
            type = ecs_dbg_get_column_type(
                world, system, failure_info.column);
            type_expr = ecs_type_to_expr(world, type);
            printf("column %d: ", failure_info.column);
        }

        switch(failure_info.reason) {
        case EcsMatchOk:
            break;
        case EcsMatchNotASystem:
            printf("entity '%s' is not a system\n", ptr);
            break;
        case EcsMatchSystemIsATask:
            printf("system is a task\n");
            break;
        case EcsMatchEntityIsDisabled:
            printf("entity is disabled\n");
            break;
        case EcsMatchEntityIsPrefab:
            printf("entity is a prefab\n");
            break;
        case EcsMatchFromSelf:
            printf("[%s] missing (owned or shared)\n", type_expr);
            break;
        case EcsMatchFromOwned:
            printf("[%s] missing (owned)\n", type_expr);
            break;
        case EcsMatchFromShared:
            printf("[%s] missing (shared)\n", type_expr);
            break;
        case EcsMatchFromContainer:
            printf("[%s] missing (container)\n", type_expr);
            break;
        case EcsMatchFromEntity:
            printf(
                "[%s] missing (from entity, system will never run!)\n", 
                type_expr);
            break;
        case EcsMatchOrFromSelf:
            printf("[%s] missing in OR expression (owned or shared)\n", type_expr);
            break;
        case EcsMatchOrFromContainer:
            printf("[%s] missing in OR expression (from container)\n", type_expr);
            break;
        case EcsMatchNotFromSelf:
            printf("has [%s] from NOT expression (owned or shared)\n", type_expr);
            break;
        case EcsMatchNotFromOwned:
            printf("has [%s] in NOT expression (owned)\n", type_expr);
            break;
        case EcsMatchNotFromShared:
            printf("has [%s] in NOT expression (shared)\n", type_expr);
            break;
        case EcsMatchNotFromContainer:
            printf("has [%s] in NOT expression (from container)\n", type_expr);
            break;
        }

        ecs_os_free(type_expr);      
    }

    return 0;
}

static
int cmd_add_remove(
    ecs_world_t *world,
    const char *args,
    bool is_remove)
{
    char arg[256];
    const char *ptr = parse_arg(args, arg);
    if (!ptr) {
        return -1;
    }

    /* Skip whitespace */
    ptr ++;

    ecs_entity_t e = parse_entity_id(world, arg);
    if (!e) {
        return -1;
    }

    ecs_type_t type;

    if (ptr[0] == '[') {
        ecs_type_filter_t filter = {0};
        if (parse_type_filter(world, ptr, &filter)) {
            return -1;
        }

        type = filter.include;
    } else {
        ecs_entity_t component = parse_entity_id(world, ptr);
        if (!component) {
            return -1;
        }

        type = ecs_type_from_entity(world, component);
    }

    char *type_expr = ecs_type_to_expr(world, type);

    if (is_remove) {
        if (!_ecs_has_owned(world, e, type)) {
            if (_ecs_has(world, e, type)) {
                printf("entity '%s' does not own [%s]\n", arg, type_expr);
            } else {
                printf("entity '%s' does not have [%s]\n", arg, type_expr);
            }
        } else {
            _ecs_remove(world, e, type);
            if (_ecs_has(world, e, type)) {
                printf("removed override [%s] from entity '%s'\n", type_expr, arg);
            } else {
                printf("removed [%s] from entity '%s'\n", type_expr, arg);
            }
        }
    } else {
        if (_ecs_has_owned(world, e, type)) {
            printf("entity '%s' already has [%s]\n", arg, type_expr);
        } else {
            if (_ecs_has(world, e, type)) {
                _ecs_add(world, e, type);
                printf("overridden [%s] for entity '%s'\n", type_expr, arg);
            } else {
                _ecs_add(world, e, type);
                printf("added [%s] to entity '%s'\n", type_expr, arg);
            }
        }
    }

    ecs_os_free(type_expr);

    return 0;
}

static
int cmd_delete(
    ecs_world_t *world,
    const char *args)
{
    ecs_entity_t e = parse_entity_id(world, args);
    if (!e) {
        return -1;
    }

    ecs_delete(world, e);
    printf("deleted entity '%s'\n", args);

    return 0;
}

static
void cmd_help()
{
    printf("Commands:\n");
    printf(" - [e]ntity entity                  - Display information about one or more matching entities\n");
    printf(" - [t]able  entity                  - Display information about one or more matching tables\n");
    printf(" - [s]ystem system                  - Display information about a matching system\n");
    printf(" - [m]atch  entity system           - Display if entity matches with system and why (not)\n");
    printf(" - [a]dd entity component           - Add component to entity\n");
    printf(" - [r]emove entity component        - Remove entity from component\n");
    printf(" - [d]elete entity                  - Delete entity\n");
    printf(" - snapshot                         - Take a snapshot of the current state\n");
    printf(" - restore                          - Restore the previous snapshot\n");
    printf("\n");
    printf(" entity can be any of the following:\n");
    printf(" - id         (e.g. 42)\n");
    printf(" - name       (e.g. MyEntity)\n");
    printf(" - expression (e.g. [Position, Velocity], matches multiple)\n");
    printf("\n");
    printf(" component, system can be any of the following:\n");
    printf(" - id         (e.g. 42)\n");
    printf(" - name       (e.g. MyEntity)\n");
    printf("\n");
    printf(" If no argument is provided for either 'entity' or 'table', all entities or tables\n");
    printf(" are shown, respectively.\n");
    printf("\n");
    printf("Examples:\n");
    printf("  entity 42\n");
    printf("  e 42\n");
    printf("  e MyEntity\n");
    printf("  e [Position, Velocity]\n");
    printf("  add 42 Position\n");
    printf("  match 42 Move\n");
    printf("\n");
}

int cmd_snapshot(
    ecs_world_t *world, 
    const char *args, 
    ui_thread_t *ctx)
{
    if (ctx->snapshot) {
        ecs_snapshot_free(world, ctx->snapshot);
    }

    if (args[0] == '[') {
        ecs_type_filter_t filter = {0};
        if (parse_type_filter(world, args, &filter)) {
            return -1;
        }

        ctx->snapshot = ecs_snapshot_take(world, &filter);
    } else {
        ctx->snapshot = ecs_snapshot_take(world, NULL);
    }

    return 0;
}

int cmd_restore(
    ecs_world_t *world,
    ui_thread_t *ctx)
{
    if (!ctx->snapshot) {
        return -1;
    }

    ecs_snapshot_restore(world, ctx->snapshot);
    
    ctx->snapshot = NULL;

    return 0;
}

static
int parse_cmd(
    ecs_world_t *world, 
    const char *cmd,
    ui_thread_t *ctx) 
{
    const char *args;

    if (!cmd[0]) {
        return 0;
    }

    if ((args = is_cmd(cmd, "table"))) {
        return cmd_table(world, args);
    } else
    if ((args = is_cmd(cmd, "system"))) {
        return cmd_system(world, args);
    } else
    if ((args = is_cmd(cmd, "entity"))) {
        return cmd_entity(world, args);
    } else
    if ((args = is_cmd(cmd, "match"))) {
        return cmd_match(world, args);
    } else
    if ((args = is_cmd(cmd, "add"))) {
        return cmd_add_remove(world, args, false);
    } else
    if ((args = is_cmd(cmd, "remove"))) {
        return cmd_add_remove(world, args, true);
    } else    
    if ((args = is_cmd(cmd, "delete"))) {
        return cmd_delete(world, args);
    } else
    if ((args = is_cmd(cmd, "help"))) {
        cmd_help();
        return 0;
    } else
    if ((args = is_cmd(cmd, "quit"))) {
        ecs_quit(world);
        return 0;
    } else
    if ((args = is_cmd(cmd, "snapshot"))) {
        return cmd_snapshot(world, args, ctx);
    } else
    if ((args = is_cmd(cmd, "restore"))) {
        return cmd_restore(world, ctx);
    }

    return -1;
}

static
void* ui_thread(void *arg) {
    ui_thread_t *ctx = arg;
    ecs_world_t *world = ctx->world;

    ecs_os_sleep(0, 100000000);

    while (true) {
        show_prompt();
        char *cmd = read_cmd(stdin);

        ecs_os_mutex_lock(ctx->mutex);
        if (parse_cmd(world, cmd, ctx)) {
            printf("error executing '%s'\n", cmd);
        }
        ecs_os_mutex_unlock(ctx->mutex);

        free(cmd);
    }

    return NULL;
}

static
void EcsStartUiThread(ecs_rows_t *rows) {
    ECS_COLUMN_COMPONENT(rows, ConsoleUiThread, 2);

    for (uint32_t i = 0; i < rows->count; i ++) {
        ui_thread_t *ctx = ecs_os_malloc(sizeof(ui_thread_t));
        ctx->world = rows->world;
        ctx->console_entity = rows->entities[i];
        ctx->mutex = ecs_os_mutex_new();
        ctx->snapshot = NULL;

        /* Lock mutex, which will prevent the thread from doing unsafe access
         * on the world */
        ecs_os_mutex_lock(ctx->mutex);

        ecs_set(
            rows->world,
            rows->entities[i],
            ConsoleUiThread, {
                .thread = ecs_os_thread_new(ui_thread, ctx),
                .ctx = ctx
            }
        );
    }
}

static
void EcsRunConsole(ecs_rows_t *rows) {
    ECS_COLUMN(rows, ConsoleUiThread, thr, 1);

    ui_thread_t *ctx = thr->ctx;

    /* Unlock and relock the mutex to give the UI thread the opportunity to
     * do operations */
    ecs_os_mutex_unlock(ctx->mutex);

    /* ui thread does something ... */
    ecs_os_sleep(0, 10000000);

    ecs_os_mutex_lock(ctx->mutex);
}

void FlecsSystemsConsoleImport(
    ecs_world_t *world,
    int flags)
{
    (void)flags;
    
    ECS_MODULE(world, FlecsSystemsConsole);

    ECS_COMPONENT(world, EcsConsole);
    ECS_COMPONENT(world, ConsoleUiThread);

    ECS_SYSTEM(world, EcsStartUiThread, EcsOnAdd, EcsConsole, .ConsoleUiThread);
    ECS_SYSTEM(world, EcsRunConsole, EcsOnStore, ConsoleUiThread);

    ecs_set_period(world, EcsRunConsole, 0.1);

    ECS_EXPORT_COMPONENT(EcsConsole);
}
