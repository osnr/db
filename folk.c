#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <assert.h>

#define JIM_EMBEDDED
#include <jim.h>

#include <pqueue.h>

#include "db.h"
#include "workqueue.h"

Db* db;

WorkQueue* workQueue;
pthread_mutex_t workQueueMutex;

__thread int threadId = -1;
int getThreadId() { return threadId; }

static Clause* jimArgsToClause(int argc, Jim_Obj *const *argv) {
    Clause* clause = malloc(SIZEOF_CLAUSE(argc - 1));
    clause->nTerms = argc - 1;
    for (int i = 1; i < argc; i++) {
        clause->terms[i - 1] = strdup(Jim_GetString(argv[i], NULL));
    }
    return clause;
}
static Clause* jimObjToClause(Jim_Interp* interp, Jim_Obj* obj) {
    int objc = Jim_ListLength(interp, obj);
    Clause* clause = malloc(SIZEOF_CLAUSE(objc));
    clause->nTerms = objc;
    for (int i = 0; i < objc; i++) {
        clause->terms[i] = strdup(Jim_GetString(Jim_ListGetIndex(interp, obj, i), NULL));
    }
    return clause;
}
static Jim_Obj* termsToJimObj(Jim_Interp* interp, int nTerms, char* terms[]) {
    Jim_Obj* termObjs[nTerms];
    for (int i = 0; i < nTerms; i++) {
        termObjs[i] = Jim_NewStringObj(interp, terms[i], -1);
    }
    return Jim_NewListObj(interp, termObjs, nTerms);
}

typedef struct EnvironmentBinding {
    char name[100];
    Jim_Obj* value;
} EnvironmentBinding;
typedef struct Environment {
    int nBindings;
    EnvironmentBinding bindings[];
} Environment;

// This function lives in main.c and not trie.c (where most
// Clause/matching logic lives) because it operates at the Tcl level,
// building up a Tcl value which may be a singleton or a list. Caller
// must free the returned Environment*.
Environment* clauseUnify(Jim_Interp* interp, Clause* a, Clause* b) {
    Environment* env = malloc(sizeof(Environment) + sizeof(EnvironmentBinding)*a->nTerms);
    env->nBindings = 0;

    for (int i = 0; i < a->nTerms; i++) {
        char aVarName[100] = {0}; char bVarName[100] = {0};
        if (trieScanVariable(a->terms[i], aVarName, sizeof(aVarName))) {
            if (aVarName[0] == '.' && aVarName[1] == '.' && aVarName[2] == '.') {
                EnvironmentBinding* binding = &env->bindings[env->nBindings++];
                memcpy(binding->name, aVarName + 3, sizeof(binding->name) - 3);
                binding->value = termsToJimObj(interp, b->nTerms - i, &b->terms[i]);
            } else if (!trieVariableNameIsNonCapturing(aVarName)) {
                EnvironmentBinding* binding = &env->bindings[env->nBindings++];
                memcpy(binding->name, aVarName, sizeof(binding->name));
                binding->value = Jim_NewStringObj(interp, b->terms[i], -1);
            }
        } else if (trieScanVariable(b->terms[i], bVarName, sizeof(bVarName))) {
            if (bVarName[0] == '.' && bVarName[1] == '.' && bVarName[2] == '.') {
                EnvironmentBinding* binding = &env->bindings[env->nBindings++];
                memcpy(binding->name, bVarName + 3, sizeof(binding->name) - 3);
                binding->value = termsToJimObj(interp, a->nTerms - i, &a->terms[i]);
            } else if (!trieVariableNameIsNonCapturing(bVarName)) {
                EnvironmentBinding* binding = &env->bindings[env->nBindings++];
                memcpy(binding->name, bVarName, sizeof(binding->name));
                binding->value = Jim_NewStringObj(interp, a->terms[i], -1);
            }
        } else if (!(a->terms[i] == b->terms[i] ||
                     strcmp(a->terms[i], b->terms[i]) == 0)) {
            free(env);
            fprintf(stderr, "clauseUnify: Unification of (%s) (%s) failed\n",
                    clauseToString(a), clauseToString(b));
            return NULL;
        }
    }
    return env;
}

// Assert! the time is 3
static int AssertFunc(Jim_Interp *interp, int argc, Jim_Obj *const *argv) {
    Clause* clause = jimArgsToClause(argc, argv);

    pthread_mutex_lock(&workQueueMutex);
    workQueuePush(workQueue, (WorkQueueItem) {
       .op = ASSERT,
       .thread = -1,
       .assert = { .clause = clause }
    });
    pthread_mutex_unlock(&workQueueMutex);

    return (JIM_OK);
}
// Retract! the time is /t/
static int RetractFunc(Jim_Interp *interp, int argc, Jim_Obj *const *argv) {
    Clause* pattern = jimArgsToClause(argc, argv);

    pthread_mutex_lock(&workQueueMutex);
    workQueuePush(workQueue, (WorkQueueItem) {
       .op = RETRACT,
       .thread = -1,
       .retract = { .pattern = pattern }
    });
    pthread_mutex_unlock(&workQueueMutex);

    return (JIM_OK);
}
// Hold! {Omar's time} {the time is 3}
__thread int64_t latestVersion = 0; // TODO: split by key?
static int HoldFunc(Jim_Interp *interp, int argc, Jim_Obj *const *argv) {
    assert(argc == 3);
    const char* key = strdup(Jim_GetString(argv[1], NULL));
    Clause* clause = jimObjToClause(interp, argv[2]);

    pthread_mutex_lock(&workQueueMutex);
    workQueuePush(workQueue, (WorkQueueItem) {
       .op = HOLD,
       .thread = -1,
       .hold = { .key = key, .version = ++latestVersion,
                 .clause = clause, }
    });
    pthread_mutex_unlock(&workQueueMutex);

    return (JIM_OK);
}

__thread MatchRef currentMatch = MATCH_REF_NULL;
static int SayFunc(Jim_Interp *interp, int argc, Jim_Obj *const *argv) {
    Clause* clause;
    int thread = -1;
    if (Jim_String(argv[1])[0] == '@') {
        clause = jimArgsToClause(argc - 1, argv + 1);
        thread = atoi(Jim_String(argv[1]));
    } else {
        clause = jimArgsToClause(argc, argv);
    }

    pthread_mutex_lock(&workQueueMutex);
    workQueuePush(workQueue, (WorkQueueItem) {
       .op = SAY,
       .thread = thread,
       .say = {
           .parent = currentMatch,
           .clause = clause,
       }
    });
    pthread_mutex_unlock(&workQueueMutex);

    return (JIM_OK);
}
static int dbQueryFunc(Jim_Interp *interp, int argc, Jim_Obj *const *argv) {
    Clause* pattern = jimArgsToClause(argc, argv);

    ResultSet* rs = dbQuery(db, pattern);

    int nResults = (int) rs->nResults;
    Jim_Obj* resultObjs[nResults];
    for (size_t i = 0; i < rs->nResults; i++) {
        Statement* result = statementAcquire(db, rs->results[i]);
        Environment* env = clauseUnify(interp, pattern, statementClause(result));
        assert(env != NULL);
        Jim_Obj* envDict[(env->nBindings + 1) * 2];
        envDict[0] = Jim_NewStringObj(interp, "__stmt", -1);
        char buf[100]; snprintf(buf, 100,  "(Statement*) 0x%p", result);
        envDict[1] = Jim_NewStringObj(interp, buf, -1);
        for (int j = 0; j < env->nBindings; j++) {
            envDict[(j+1)*2] = Jim_NewStringObj(interp, env->bindings[j].name, -1);
            envDict[(j+1)*2+1] = env->bindings[j].value;
        }

        resultObjs[i] = Jim_NewDictObj(interp, envDict, (env->nBindings + 1) * 2);
        free(env);
        statementRelease(result);
    }
    free(pattern);
    free(rs);
    Jim_SetResult(interp, Jim_NewListObj(interp, resultObjs, nResults));
    return JIM_OK;
}
static int __scanVariableFunc(Jim_Interp *interp, int argc, Jim_Obj *const *argv) {
    assert(argc == 2);
    char varName[100];
    if (trieScanVariable(Jim_String(argv[1]), varName, 100)) {
        Jim_SetResultString(interp, varName, strlen(varName));
    } else {
        Jim_SetResultBool(interp, false);
    }
    return JIM_OK;
}
static int __variableNameIsNonCapturingFunc(Jim_Interp *interp, int argc, Jim_Obj *const *argv) {
    assert(argc == 2);
    Jim_SetResultBool(interp, trieVariableNameIsNonCapturing(Jim_String(argv[1])));
    return JIM_OK;
}
static int __startsWithDollarSignFunc(Jim_Interp *interp, int argc, Jim_Obj *const *argv) {
    assert(argc == 2);
    Jim_SetResultBool(interp, Jim_String(argv[1])[0] == '$');
    return JIM_OK;
}
static int __dbFunc(Jim_Interp *interp, int argc, Jim_Obj *const *argv) {
    char ret[100]; snprintf(ret, 100, "(Db*) %p", db);
    Jim_SetResultString(interp, ret, strlen(ret));
    return JIM_OK;
}
static int __threadIdFunc(Jim_Interp *interp, int argc, Jim_Obj *const *argv) {
    Jim_SetResultInt(interp, threadId);
    return JIM_OK;
}
static int __exitFunc(Jim_Interp *interp, int argc, Jim_Obj *const *argv) {
    assert(argc == 2);
    long exitCode; Jim_GetLong(interp, argv[1], &exitCode);
    exit(exitCode);
    return JIM_OK;
}

__thread Jim_Interp* interp = NULL;
static void interpBoot() {
    interp = Jim_CreateInterp();
    Jim_RegisterCoreCommands(interp);
    Jim_InitStaticExtensions(interp);
    Jim_CreateCommand(interp, "Assert!", AssertFunc, NULL, NULL);
    Jim_CreateCommand(interp, "Retract!", RetractFunc, NULL, NULL);
    Jim_CreateCommand(interp, "Hold!", HoldFunc, NULL, NULL);
    Jim_CreateCommand(interp, "Say", SayFunc, NULL, NULL);
    Jim_CreateCommand(interp, "Query!", dbQueryFunc, NULL, NULL);
    Jim_CreateCommand(interp, "__scanVariable", __scanVariableFunc, NULL, NULL);
    Jim_CreateCommand(interp, "__variableNameIsNonCapturing", __variableNameIsNonCapturingFunc, NULL, NULL);
    Jim_CreateCommand(interp, "__startsWithDollarSign", __startsWithDollarSignFunc, NULL, NULL);
    Jim_CreateCommand(interp, "__db", __dbFunc, NULL, NULL);
    Jim_CreateCommand(interp, "__threadId", __threadIdFunc, NULL, NULL);
    Jim_CreateCommand(interp, "__exit", __exitFunc, NULL, NULL);
    Jim_EvalFile(interp, "prelude.tcl");
}
static void eval(const char* code) {
    if (interp == NULL) { interpBoot(); }

    int error = Jim_Eval(interp, code);
    if (error == JIM_ERR) {
        Jim_MakeErrorMessage(interp);
        fprintf(stderr, "eval: (%s) -> (%s)\n", code, Jim_GetString(Jim_GetResult(interp), NULL));
        Jim_FreeInterp(interp);
        exit(EXIT_FAILURE);
    }
}

//////////////////////////////////////////////////////////
// Evaluator
//////////////////////////////////////////////////////////

// Releases when and stmt before doing the Tcl evaluation.  Rule: you
// should never be holding a lock while doing a Tcl evaluation.
static void runWhenBlock(Statement* when, Clause* whenPattern, Statement* stmt) {
    // TODO: Free clauseToString
    /* printf("runWhenBlock:\n  When: (%s)\n  Stmt: (%s)\n", */
    /*        clauseToString(statementClause(when)), */
    /*        clauseToString(statementClause(stmt))); */

    Clause* whenClause = statementClause(when);
    assert(whenClause->nTerms >= 6);

    // when the time is /t/ /lambda/ with environment /builtinEnv/
    const char* lambda = whenClause->terms[whenClause->nTerms - 4];
    const char* builtinEnv = whenClause->terms[whenClause->nTerms - 1];

    Jim_Obj* expr = Jim_NewStringObj(interp, builtinEnv, -1);

    // Prepend `apply $lambda` to expr:
    Jim_Obj* applyLambda[] = {
        Jim_NewStringObj(interp, "apply", -1),
        Jim_NewStringObj(interp, lambda, -1)
    };
    Jim_ListInsertElements(interp, expr, 0,
                           sizeof(applyLambda)/sizeof(applyLambda[0]),
                           applyLambda);

    // Postpend bound variables to expr:
    Environment* env = clauseUnify(interp, whenPattern, statementClause(stmt));
    assert(env != NULL);
    Jim_Obj* envArgs[env->nBindings];
    for (int i = 0; i < env->nBindings; i++) {
        envArgs[i] = env->bindings[i].value;
    }
    Jim_ListInsertElements(interp, expr,
                           Jim_ListLength(interp, expr),
                           env->nBindings, envArgs);
    free(env);

    StatementRef parents[] = { statementRef(db, when), statementRef(db, stmt) };
    statementRelease(when);
    statementRelease(stmt);

    currentMatch = dbInsertMatch(db, 2, parents);

    int error = Jim_EvalObj(interp, expr);

    if (error == JIM_ERR) {
        Jim_MakeErrorMessage(interp);
        fprintf(stderr, "runWhenBlock: (%.100s) -> (%s)\n",
                lambda,
                Jim_GetString(Jim_GetResult(interp), NULL));
        /* Jim_FreeInterp(interp); */
        /* exit(EXIT_FAILURE); */
    }
}
static void pushRunWhenBlock(StatementRef when, Clause* whenPattern, StatementRef stmt) {
    pthread_mutex_lock(&workQueueMutex);
    workQueuePush(workQueue, (WorkQueueItem) {
       .op = RUN,
       .thread = -1,
       .run = { .when = when, .whenPattern = whenPattern, .stmt = stmt }
    });
    pthread_mutex_unlock(&workQueueMutex);
}

// Prepends `/someone/ claims` to `clause`. Returns NULL if `clause`
// shouldn't be claimized. Returns a new heap-allocated Clause* that
// must be freed by the caller.
static Clause* claimizeClause(Clause* clause) {
    if (clause->nTerms >= 2 &&
        (strcmp(clause->terms[1], "claims") == 0 ||
         strcmp(clause->terms[1], "wishes") == 0)) {
        return NULL;
    }

    // the time is /t/ -> /someone/ claims the time is /t/
    Clause* ret = malloc(SIZEOF_CLAUSE(2 + clause->nTerms));
    ret->nTerms = 2 + clause->nTerms;
    ret->terms[0] = "/someone/"; ret->terms[1] = "claims";
    for (int i = 0; i < clause->nTerms; i++) {
        ret->terms[2 + i] = clause->terms[i];
    }
    return ret;
}
static Clause* unclaimizeClause(Clause* clause) {
    // Omar claims the time is 3
    //   -> the time is 3
    Clause* ret = malloc(SIZEOF_CLAUSE(clause->nTerms - 2));
    ret->nTerms = 0;
    for (int i = 2; i < clause->nTerms; i++) {
        ret->terms[ret->nTerms++] = clause->terms[i];
    }
    return ret;
}
static Clause* whenizeClause(Clause* clause) {
    // the time is /t/
    //   -> when the time is /t/ /__lambda/ with environment /__env/
    Clause* ret = malloc(SIZEOF_CLAUSE(clause->nTerms + 5));
    ret->nTerms = clause->nTerms + 5;
    ret->terms[0] = "when";
    for (int i = 0; i < clause->nTerms; i++) {
        ret->terms[1 + i] = clause->terms[i];
    }
    ret->terms[1 + clause->nTerms] = "/__lambda/";
    ret->terms[2 + clause->nTerms] = "with";
    ret->terms[3 + clause->nTerms] = "environment";
    ret->terms[4 + clause->nTerms] = "/__env/";
    return ret;
}
static Clause* unwhenizeClause(Clause* whenClause) {
    // when the time is /t/ /lambda/ with environment /env/
    //   -> the time is /t/
    Clause* ret = malloc(SIZEOF_CLAUSE(whenClause->nTerms - 5));
    ret->nTerms = 0;
    for (int i = 1; i < whenClause->nTerms - 4; i++) {
        ret->terms[ret->nTerms++] = whenClause->terms[i];
    }
    return ret;
}

// React to the addition of a new statement: fire any pertinent
// existing Whens & if the new statement is a When, then fire it with
// respect to any pertinent existing statements.
static void reactToNewStatement(StatementRef ref) {
    Statement* stmt = statementAcquire(db, ref);
    if (stmt == NULL) { return; }

    Clause* clause = statementClause(stmt);

    // TODO: implement collected matches

    if (strcmp(clause->terms[0], "when") == 0)  {
        // Find the query pattern of the when:
        Clause* pattern = unwhenizeClause(clause);

        // Scan the existing statement set for any already-existing
        // matching statements.
        ResultSet* existingMatchingStatements = dbQuery(db, pattern);
        for (int i = 0; i < existingMatchingStatements->nResults; i++) {
            printf("push-run-when-block\n");
            pushRunWhenBlock(ref, pattern,
                             existingMatchingStatements->results[i]);
        }
        free(existingMatchingStatements);

        Clause* claimizedPattern = claimizeClause(pattern);
        if (claimizedPattern) {
            existingMatchingStatements = dbQuery(db, claimizedPattern);
            for (int i = 0; i < existingMatchingStatements->nResults; i++) {
                pushRunWhenBlock(ref, claimizedPattern,
                                 existingMatchingStatements->results[i]);
            }
            free(existingMatchingStatements);
        }
        // free(pattern);
    }

    // Trigger any already-existing reactions to the addition of this
    // statement (look for Whens that are already in the database).
    {
        // the time is 3
        //   -> when the time is 3 /__lambda/ with environment /__env/
        Clause* whenizedClause = whenizeClause(clause);

        ResultSet* existingReactingWhens = dbQuery(db, whenizedClause);
        // TODO: lease result statements so they don't get freed?
        // hazard pointer?
        for (int i = 0; i < existingReactingWhens->nResults; i++) {
            StatementRef whenRef = existingReactingWhens->results[i];
            // when the time is /t/ /__lambda/ with environment /__env/
            //   -> the time is /t/
            Statement* when = statementAcquire(db, whenRef);
            Clause* whenPattern = unwhenizeClause(statementClause(when));
            statementRelease(when);

            pushRunWhenBlock(whenRef, whenPattern, ref);
        }
        free(existingReactingWhens);
    }
    if (clause->nTerms >= 2 && strcmp(clause->terms[1], "claims") == 0) {
        // Cut off `/x/ claims` from start of clause:
        //
        // /x/ claims the time is 3
        //   -> when the time is 3 /__lambda/ with environment /__env/
        Clause* unclaimizedClause = unclaimizeClause(clause);
        Clause* whenizedUnclaimizedClause = whenizeClause(unclaimizedClause);

        ResultSet* existingReactingWhens = dbQuery(db, whenizedUnclaimizedClause);
        // TODO: lease result statements so they don't get freed?
        // hazard pointer?
        free(unclaimizedClause);
        free(whenizedUnclaimizedClause);

        for (int i = 0; i < existingReactingWhens->nResults; i++) {
            StatementRef whenRef = existingReactingWhens->results[i];
            // when the time is /t/ /__lambda/ with environment /__env/
            //   -> /someone/ claims the time is /t/
            Statement* when = statementAcquire(db, whenRef);
            Clause* unwhenizedWhenPattern = unwhenizeClause(statementClause(when));
            Clause* claimizedUnwhenizedWhenPattern = claimizeClause(unwhenizedWhenPattern);
            statementRelease(when);

            pushRunWhenBlock(whenRef, claimizedUnwhenizedWhenPattern, ref);
            free(unwhenizedWhenPattern);
        }
        free(existingReactingWhens);
    }

    statementRelease(stmt);
}

static void reactToRemovedStatement(StatementRef ref);

static void reactToRemovedMatch(MatchRef ref) {
    Match* match = matchAcquire(db, ref);
    if (match == NULL) { return; }

    // Walk through edges to statements:
    for (MatchEdgeIterator it = matchEdgesBegin(match);
         !matchEdgesIsEnd(it);
         it = matchEdgesNext(it)) {

        switch (matchEdgeType(it)) {
        case EDGE_EMPTY: break;
        case EDGE_PARENT: {
            // This match is dead, so remove edges to the match from
            // all of its co-parent statements.
            Statement* parent = statementAcquire(db, matchEdgeStatement(it));
            statementRemoveEdgeToMatch(parent, EDGE_CHILD, matchRef(db, match));
            statementRelease(parent);
            break;
        }
        case EDGE_CHILD: {
            Statement* child = statementAcquire(db, matchEdgeStatement(it));
            if (statementRemoveEdgeToMatch(child, EDGE_PARENT, matchRef(db, match)) == 0) {
                // This child statement now has no remaining parent
                // matches to prop it up, so it's dead and should be
                // deindexed, its removal reacted to, and then freed.

                /* printf("reactToRemovedMatch: dead statement: %p (%d terms: %.100s)\n", child, statementClause(child)->nTerms, clauseToString(statementClause(child))); */
                free(dbQueryAndDeindexStatements(db, statementClause(child)));
                statementRelease(child);
                reactToRemovedStatement(matchEdgeStatement(it));
            } else {
                statementRelease(child);
            }
        } }
    }

    matchFree(match);
    matchRelease(match);
}

// Must be called with the database lock held.
static void reactToRemovedStatement(StatementRef ref) {
    Statement* stmt = statementAcquire(db, ref);
    if (stmt == NULL) { return; }

    // Walk through edges to matches:
    for (StatementEdgeIterator it = statementEdgesBegin(stmt);
         !statementEdgesIsEnd(it);
         it = statementEdgesNext(it)) {

        switch (statementEdgeType(it)) {
        case EDGE_EMPTY: break;
        case EDGE_PARENT: {
            // remove edge from parent match to stmt?
            // do we need this? when is a statement removed that has a parent match?
            printf("reactToRemovedStatement: Remove edge from parent match to me\n");
            // lock and retry?
            break;
        }
        case EDGE_CHILD: {
            // A child match is removed if _any_ of its parent
            // statements is removed, so this match must be removed.
            reactToRemovedMatch(statementEdgeMatch(it));
            break;
        } }
    }

    statementFree(stmt);
    statementRelease(stmt);
}
void workerRun(WorkQueueItem item) {
    if (item.op == ASSERT) {
        /* printf("Assert (%s)\n", clauseToString(item.assert.clause)); */

        StatementRef ref;
        ref = dbInsertStatement(db, item.assert.clause, 0, NULL);
        reactToNewStatement(ref);

    } else if (item.op == RETRACT) {
        /* printf("Retract (%s)\n", clauseToString(item.retract.pattern)); */

        // This removes the statements from the lookup index, so they
        // won't appear as query results, but it doesn't disconnect
        // them or free them.
        ResultSet* retractStmts = dbQueryAndDeindexStatements(db, item.retract.pattern);

        for (int i = 0; i < retractStmts->nResults; i++) {
            reactToRemovedStatement(retractStmts->results[i]);
        }
        free(retractStmts);

    } else if (item.op == HOLD) {
        StatementRef oldRef;
        StatementRef newRef;
        newRef = dbHoldStatement(db, item.hold.key, item.hold.version,
                                 item.hold.clause,
                                 &oldRef);
        reactToNewStatement(newRef);
        reactToRemovedStatement(oldRef);

    } else if (item.op == SAY) {
        printf("@%d: Say (%.100s)\n", threadId, clauseToString(item.say.clause));

        StatementRef ref;
        ref = dbInsertStatement(db, item.say.clause, 1, &item.say.parent);
        reactToNewStatement(ref);

    } else if (item.op == RUN) {
        printf("RUNRUNRUN\n");

        // TODO: dereference refs. if any fail, then die
        // Run statement
        Statement* when = statementAcquire(db, item.run.when);
        Statement* stmt = statementAcquire(db, item.run.stmt);
        if (when == NULL || stmt == NULL) {
            printf("Run failed\n");
        } else {
            printf("@%d: Run (%.100s)\n", threadId, clauseToString(statementClause(when)));
            runWhenBlock(when, item.run.whenPattern, stmt);
        }

        /* dbRelease(when); */
        /* dbRelease(stmt); */

        // TODO: free whenPattern
        
    } else {
        fprintf(stderr, "workerRun: Unknown work item op: %d\n",
                item.op);
        exit(1);
    }
}
void* workerMain(void* arg) {
    threadId = (int) (intptr_t) arg;

    interpBoot();

    for (;;) {
        pthread_mutex_lock(&workQueueMutex);
        WorkQueueItem item = workQueuePop(workQueue);
        if (item.op != NONE &&
            item.thread != -1 &&
            item.thread != threadId) {

            // Skip and requeue.
            workQueuePush(workQueue, item);
            item.op = NONE;
        }
        pthread_mutex_unlock(&workQueueMutex);

        // TODO: if item is none, then sleep or wait on condition
        // variable.
        if (item.op == NONE) { usleep(100000); continue; }

        /* printf("Worker %d: ", threadId, item.op); */
        workerRun(item);
    }
}



static void trieWriteToPdf(Trie* trie) {
    char code[500];
    snprintf(code, 500,
             "source db.tcl; "
             "trieWriteToPdf {(Trie*) %p} trie.pdf; puts trie.pdf", trie);
    eval(code);
}
static void dbWriteToPdf(Db* db) {
    char code[500];
    snprintf(code, 500,
             "source db.tcl; "
             "dbWriteToPdf {(Db*) %p} db.pdf; puts db.pdf", db);
    eval(code);
}
static void exitHandler() {
    trieWriteToPdf(dbGetClauseToStatementId(db));
    dbWriteToPdf(db);
}
int main(int argc, char** argv) {
    // Do all setup.

    // Set up database.
    db = dbNew();

    // Set up workqueue.
    workQueue = workQueueNew();
    pthread_mutex_init(&workQueueMutex, NULL);

    if (argc == 1) {
        eval("source boot.folk");
    } else {
        char code[100]; snprintf(code, 100, "source %s", argv[1]);
        eval(code);
    }

    // Spawn NTHREADS workers. (Worker 0 is this main thread itself,
    // which needs to be an active worker, in case we need to do
    // things like GLFW that the OS forces to be on the main thread.)
    const int NTHREADS = 5;
    pthread_t th[NTHREADS];
    for (int i = 1; i < NTHREADS; i++) {
        pthread_create(&th[i], NULL, workerMain, (void*) (intptr_t) i);
    }
    workerMain(0);

    atexit(exitHandler);
    for (int i = 0; i < NTHREADS; i++) {
        pthread_join(th[i], NULL);
    }
}