/*
 * PHPStanTurbo\FileAnalyserCallback — native implementation of
 * PHPStan\Analyser\FileAnalyserCallback.
 *
 * Not final: the generated stub PHPStan\Analyser\FileAnalyserCallback extends
 * this class, and FileAnalyser instantiates that stub. State lives in the PHP
 * object's properties, like the twin's.
 *
 * __invoke() runs once per visited AST node — 269k times for src/Type alone,
 * millions for a real project — and the work it does itself is bookkeeping
 * around ~24 userland calls per node (the rules, the collectors, the
 * dependency resolver, the error transformer). That bookkeeping is what the
 * port absorbs: the __invoke frame, the four instanceof probes, get_class(),
 * the property fetches, the two foreach loops and the array appends. Every
 * call it makes stays a call into PHP — the rules are the analysis, and they
 * keep running as PHP.
 *
 * The recursion of the twin is kept method for method, including the order of
 * the userland calls, so an exception thrown by any of them surfaces exactly
 * where it does without the extension.
 */

#include "support.h"
#include "zv.h"

/* declaration order in pt_register_file_analyser_callback() */
#define PT_FAC_PROP_FILE 0
#define PT_FAC_PROP_ANALYSED_FILES 1
#define PT_FAC_PROP_RULE_REGISTRY 2
#define PT_FAC_PROP_COLLECTOR_REGISTRY 3
#define PT_FAC_PROP_OUTER_NODE_CALLBACK 4
#define PT_FAC_PROP_PARSER_NODES 5
#define PT_FAC_PROP_IGNORE_ERROR_EXTENSIONS 6
#define PT_FAC_PROP_PARSER 7
#define PT_FAC_PROP_DEPENDENCY_RESOLVER 8
#define PT_FAC_PROP_PACKAGE_DEPENDENCY_RESOLVER 9
#define PT_FAC_PROP_RULE_ERROR_TRANSFORMER 10
#define PT_FAC_PROP_PROCESSED_FILES 11
#define PT_FAC_PROP_FILE_ERRORS 12
#define PT_FAC_PROP_FILE_COLLECTED_DATA 13
#define PT_FAC_PROP_FILE_DEPENDENCIES 14
#define PT_FAC_PROP_USED_TRAIT_FILE_DEPENDENCIES 15
#define PT_FAC_PROP_FILE_PACKAGE_DEPENDENCIES 16
#define PT_FAC_PROP_EXPORTED_NODES 17
#define PT_FAC_PROP_TEMPORARY_FILE_ERRORS 18
#define PT_FAC_PROP_LINES_TO_IGNORE 19
#define PT_FAC_PROP_UNMATCHED_LINE_IGNORES 20

/* method names of the userland calls, plus the one node attribute read;
 * permanent interned strings, so the function-table lookups hash nothing */
static struct {
	zend_string *process_node;
	zend_string *get_rules;
	zend_string *get_collectors;
	zend_string *transform;
	zend_string *should_ignore;
	zend_string *can_be_ignored;
	zend_string *resolve_dependencies;
	zend_string *resolve_used_trait_dependencies;
	zend_string *get_file_dependencies;
	zend_string *get_file_paths;
	zend_string *get_non_analysed_dependencies;
	zend_string *get_exported_node;
	zend_string *get_file;
	zend_string *get_file_description;
	zend_string *is_in_trait;
	zend_string *get_trait_reflection;
	zend_string *get_file_name;
	zend_string *parse_file;
	zend_string *get_start_line;
	zend_string *get_end_line;
	zend_string *get_original_node;
	zend_string *get_collector_type;
	zend_string *get_data;
	zend_string *get_message;
	zend_string *get_tip;
	zend_string *get_identifier;
	zend_string *get_name;
	zend_string *get_trace_as_string;
	zend_string *prepare_trace;
	zend_string *with_identifier;
	zend_string *with_metadata;
	zend_string *lines_to_ignore_attribute;
	zend_string *files_key;
	zend_string *packages_key;
} pt_fac_str;

/*
 * One-entry inline caches for the method lookups: without them every call
 * this class makes into PHP pays a function-table lookup the VM's own call
 * path gets for free from its run-time cache. The fixed call sites see the
 * same class every time — the registries, the resolvers and the transformer
 * are one object each for a whole run — while Rule::processNode() and
 * Collector::processNode() see hundreds of classes, so those go through a
 * direct-mapped table indexed by the class entry's address.
 *
 * Class entries do not outlive the request; both caches are dropped in
 * RSHUTDOWN.
 */
struct pt_fac_call_site {
	zend_class_entry *ce;
	zend_function *fn;
};

enum {
	PT_FAC_SITE_GET_RULES = 0,
	PT_FAC_SITE_GET_COLLECTORS,
	PT_FAC_SITE_TRANSFORM,
	PT_FAC_SITE_RESOLVE_DEPENDENCIES,
	PT_FAC_SITE_RESOLVE_USED_TRAIT_DEPENDENCIES,
	PT_FAC_SITE_GET_FILE,
	PT_FAC_SITE_IS_IN_TRAIT,
	PT_FAC_SITE_GET_FILE_DEPENDENCIES,
	PT_FAC_SITE_GET_FILE_PATHS,
	PT_FAC_SITE_GET_NON_ANALYSED_DEPENDENCIES,
	PT_FAC_SITE_GET_EXPORTED_NODE,
	PT_FAC_SITE_CAN_BE_IGNORED,
	PT_FAC_SITE_SHOULD_IGNORE,
	PT_FAC_SITE_COUNT
};

#define PT_FAC_PROCESS_NODE_CACHE_LIMIT 256

static pt_fac_call_site pt_fac_sites[PT_FAC_SITE_COUNT];
static pt_fac_call_site pt_fac_process_node_cache[PT_FAC_PROCESS_NODE_CACHE_LIMIT];

namespace phpstanturbo {

/* Mirrors PHPStan\Analyser\FileAnalyserCallback. */
class FileAnalyserCallback
{
public:
	explicit FileAnalyserCallback(zval *self) : self(Z_OBJ_P(self)) {}

	/* false = pending exception */
	bool construct(zval *args)
	{
		for (uint32_t slot = PT_FAC_PROP_FILE; slot <= PT_FAC_PROP_PROCESSED_FILES; slot++) {
			zv::ObjRef(self).propAtWrite(slot, zv::Val::copyOf(zv::Ref(&args[slot])));
		}

		/* $this->linesToIgnore = $this->unmatchedLineIgnores =
		 *     [$file => $this->getLinesToIgnoreFromTokens($parserNodes)]; */
		zv::Val lines = getLinesToIgnoreFromTokens(zv::ArrRef(&args[PT_FAC_PROP_PARSER_NODES]));
		if (UNEXPECTED(lines.isUndef())) {
			return false;
		}
		zv::Arr perFile = zv::Arr::create(1);
		perFile.set(Z_STR(args[PT_FAC_PROP_FILE]), std::move(lines));
		/* one array behind both properties, as the chained assignment leaves it */
		zv::ObjRef(self).propAtWrite(PT_FAC_PROP_UNMATCHED_LINE_IGNORES, zv::Val::copyOf(perFile.ref()));
		zv::ObjRef(self).propAtWrite(PT_FAC_PROP_LINES_TO_IGNORE, zv::Val(std::move(perFile)));

		return true;
	}

	/* false = pending exception */
	bool invoke(zval *node, zval *scope)
	{
		zend_object *nodeObj = Z_OBJ_P(node);

		zend_class_entry *ce = pt_class(PT_CLASS_EMIT_COLLECTED_DATA_NODE);
		if (UNEXPECTED(ce == NULL)) {
			return false;
		}
		if (instanceof_function(nodeObj->ce, ce)) {
			return collectEmittedData(nodeObj, scope);
		}

		/* $parserNodes = $this->parserNodes — the local the error transformer
		 * gets, replaced below while analysing a trait */
		zv::Val parserNodes = zv::Val::copyOf(prop(PT_FAC_PROP_PARSER_NODES));

		if (UNEXPECTED(!enterTraitNodes(nodeObj, scope, parserNodes))) {
			return false;
		}

		zv::Ref outerNodeCallback = prop(PT_FAC_PROP_OUTER_NODE_CALLBACK);
		if (!outerNodeCallback.isNull()) {
			zval args[2];
			ZVAL_COPY_VALUE(&args[0], node);
			ZVAL_COPY_VALUE(&args[1], scope);
			zval retval;
			ZVAL_UNDEF(&retval);
			bool ok = call_user_function(NULL, NULL, outerNodeCallback.raw(), &retval, 2, args) == SUCCESS;
			zval_ptr_dtor(&retval);
			if (UNEXPECTED(!ok || EG(exception))) {
				return false;
			}
		}

		/* the AnalysedCodeException messages already reported for this node,
		 * shared by the rule and the collector loop as in the twin */
		zv::ScratchTable uniquedMessages(0);

		zval nodeType;
		/* get_class($node) */
		ZVAL_STR(&nodeType, nodeObj->ce->name);

		if (UNEXPECTED(!runRules(node, scope, &nodeType, parserNodes.ref(), uniquedMessages))) {
			return false;
		}
		if (UNEXPECTED(!runCollectors(node, scope, &nodeType, uniquedMessages))) {
			return false;
		}
		if (UNEXPECTED(!resolveDependencies(node, scope))) {
			return false;
		}

		ce = pt_class(PT_CLASS_IN_CLASS_NODE);
		if (UNEXPECTED(ce == NULL)) {
			return false;
		}
		if (!instanceof_function(nodeObj->ce, ce)) {
			return true;
		}

		return resolveUsedTraitDependencies(node, scope);
	}

	zv::Val getFileErrors() const { return copyProp(PT_FAC_PROP_FILE_ERRORS); }
	zv::Val getFileCollectedData() const { return copyProp(PT_FAC_PROP_FILE_COLLECTED_DATA); }
	zv::Val getFileDependencies() const { return copyProp(PT_FAC_PROP_FILE_DEPENDENCIES); }
	zv::Val getPackageDependencies() const { return copyProp(PT_FAC_PROP_FILE_PACKAGE_DEPENDENCIES); }
	zv::Val getUsedTraitFileDependencies() const { return copyProp(PT_FAC_PROP_USED_TRAIT_FILE_DEPENDENCIES); }
	zv::Val getExportedNodes() const { return copyProp(PT_FAC_PROP_EXPORTED_NODES); }
	zv::Val getLinesToIgnore() const { return copyProp(PT_FAC_PROP_LINES_TO_IGNORE); }
	zv::Val getUnmatchedLineIgnores() const { return copyProp(PT_FAC_PROP_UNMATCHED_LINE_IGNORES); }
	zv::Val getTemporaryFileErrors() const { return copyProp(PT_FAC_PROP_TEMPORARY_FILE_ERRORS); }
	zv::Val getProcessedFiles() const { return copyProp(PT_FAC_PROP_PROCESSED_FILES); }

private:
	zend_object *self;

	zv::Ref prop(uint32_t slot) const { return zv::Ref(OBJ_PROP_NUM(self, slot)); }

	zv::Val copyProp(uint32_t slot) const { return zv::Val::copyOf(prop(slot)); }

	/* the property array, separated for writing ($this->foo[] = ... ) */
	zval *writableArrayProp(uint32_t slot) const
	{
		zval *slotZv = OBJ_PROP_NUM(self, slot);
		SEPARATE_ARRAY(slotZv);
		return slotZv;
	}

	/* $this->foo[] = $value */
	void pushProp(uint32_t slot, zv::Val value) const
	{
		zval v = value.take();
		zend_hash_next_index_insert(Z_ARRVAL_P(writableArrayProp(slot)), &v);
	}

	/* foreach ($values as $value) { $this->foo[] = $value; } */
	void pushAllProp(uint32_t slot, zv::Ref values) const
	{
		if (values.raw() == NULL || UNEXPECTED(!values.isArray()) || zend_hash_num_elements(values.asArrayTable()) == 0) {
			return;
		}
		HashTable *target = Z_ARRVAL_P(writableArrayProp(slot));
		for (auto entry : zv::ArrRef(values.raw())) {
			zval v;
			ZVAL_COPY(&v, entry.value().raw());
			zend_hash_next_index_insert(target, &v);
		}
	}

	/*
	 * $nodes[0]->getAttribute('linesToIgnore', []) — the twin's private
	 * getLinesToIgnoreFromTokens(), reading the attribute off the node the
	 * way the rest of the extension does. UNDEF = pending exception.
	 */
	static zv::Val getLinesToIgnoreFromTokens(zv::ArrRef nodes)
	{
		zv::Ref first = nodes.findIndex(0);
		if (first.raw() == NULL) {
			return zv::Val(zv::Arr::empty());
		}
		zv::Ref node = first.deref();
		if (UNEXPECTED(!node.isObject())) {
			zend_type_error("phpstan_turbo: parser nodes must be PhpParser\\Node instances");
			return zv::Val();
		}
		zval *attribute = pt_node_attribute(node.asObject(), pt_fac_str.lines_to_ignore_attribute);
		if (attribute == NULL) {
			return zv::Val(zv::Arr::empty());
		}

		return zv::Val::copyOf(zv::Ref(attribute));
	}

	/*
	 * $this->fileCollectedData[$scope->getFile()][$node->getCollectorType()][] = $node->getData();
	 * in the twin's evaluation order.
	 */
	bool collectEmittedData(zend_object *nodeObj, zval *scope)
	{
		zv::Val file, collectorType, data;
		if (UNEXPECTED(!callMethodAt(PT_FAC_SITE_GET_FILE, Z_OBJ_P(scope), pt_fac_str.get_file, 0, NULL, file))
			|| UNEXPECTED(!callMethod(nodeObj, pt_fac_str.get_collector_type, 0, NULL, collectorType))
			|| UNEXPECTED(!callMethod(nodeObj, pt_fac_str.get_data, 0, NULL, data))
		) {
			return false;
		}

		return appendCollectedData(file.ref(), collectorType.ref(), std::move(data));
	}

	/* $this->fileCollectedData[$file][$key][] = $data */
	bool appendCollectedData(zv::Ref file, zv::Ref key, zv::Val data)
	{
		if (UNEXPECTED(!file.isString() || !key.isString())) {
			return typeError("collected data must be keyed by strings");
		}
		zval *perFile = dimArray(writableArrayProp(PT_FAC_PROP_FILE_COLLECTED_DATA), file.asString());
		if (UNEXPECTED(perFile == NULL)) {
			return false;
		}
		zval *perCollector = dimArray(perFile, key.asString());
		if (UNEXPECTED(perCollector == NULL)) {
			return false;
		}
		zval v = data.take();
		zend_hash_next_index_insert(Z_ARRVAL_P(perCollector), &v);

		return true;
	}

	/* $array[$key] ??= []; returns the separated inner array, NULL on a type error */
	static zval *dimArray(zval *array, zend_string *key)
	{
		zval *inner = zend_symtable_find(Z_ARRVAL_P(array), key);
		if (inner == NULL) {
			zval empty;
			array_init(&empty);
			return zend_symtable_update(Z_ARRVAL_P(array), key, &empty);
		}
		ZVAL_DEREF(inner);
		if (UNEXPECTED(Z_TYPE_P(inner) != IS_ARRAY)) {
			zend_type_error("phpstan_turbo: collected data entry is not an array");
			return NULL;
		}
		SEPARATE_ARRAY(inner);

		return inner;
	}

	/*
	 * The three trait branches of the twin, in its order: the ignores of a
	 * trait declaration, the ignores a trait contributes to the file it is
	 * used in, and the trait's own nodes taking over the local $parserNodes.
	 */
	bool enterTraitNodes(zend_object *nodeObj, zval *scope, zv::Val &parserNodes)
	{
		zend_class_entry *ce = pt_class(PT_CLASS_TRAIT_STMT);
		if (UNEXPECTED(ce == NULL)) {
			return false;
		}
		if (instanceof_function(nodeObj->ce, ce) && UNEXPECTED(!matchTraitLineIgnores(nodeObj))) {
			return false;
		}

		ce = pt_class(PT_CLASS_IN_TRAIT_NODE);
		if (UNEXPECTED(ce == NULL)) {
			return false;
		}
		if (instanceof_function(nodeObj->ce, ce) && UNEXPECTED(!addTraitLineIgnores(nodeObj, scope))) {
			return false;
		}

		zv::Val inTrait;
		if (UNEXPECTED(!callMethodAt(PT_FAC_SITE_IS_IN_TRAIT, Z_OBJ_P(scope), pt_fac_str.is_in_trait, 0, NULL, inTrait))) {
			return false;
		}
		if (!inTrait.ref().isTrue()) {
			return true;
		}

		zv::Val traitReflection, traitFileName;
		if (UNEXPECTED(!callMethod(Z_OBJ_P(scope), pt_fac_str.get_trait_reflection, 0, NULL, traitReflection))) {
			return false;
		}
		if (UNEXPECTED(!traitReflection.ref().isObject())) {
			return typeError("getTraitReflection() did not return an object");
		}
		if (UNEXPECTED(!callMethod(traitReflection.ref().asObject(), pt_fac_str.get_file_name, 0, NULL, traitFileName))) {
			return false;
		}
		if (traitFileName.ref().isNull()) {
			return true;
		}

		zval arg;
		ZVAL_COPY_VALUE(&arg, traitFileName.raw());

		return callMethod(Z_OBJ_P(prop(PT_FAC_PROP_PARSER).raw()), pt_fac_str.parse_file, 1, &arg, parserNodes);
	}

	/*
	 * The lines the trait declaration covers are ignores of the trait's own
	 * file, matched there rather than in the file using it.
	 */
	bool matchTraitLineIgnores(zend_object *nodeObj)
	{
		zend_string *file = Z_STR_P(prop(PT_FAC_PROP_FILE).raw());
		zval *perFile = zend_symtable_find(Z_ARRVAL_P(prop(PT_FAC_PROP_LINES_TO_IGNORE).raw()), file);
		if (perFile == NULL || Z_TYPE_P(perFile) != IS_ARRAY) {
			return true;
		}

		/* array_keys() hands the loop a snapshot; the unsets below separate
		 * the table this iterates, which starts out shared with it */
		zv::Arr lines = zv::Arr::copyOfTable(Z_ARRVAL_P(perFile));
		for (auto entry : lines.arrRef()) {
			zval line;
			if (entry.stringKeyOrNull() != NULL) {
				ZVAL_STR(&line, entry.stringKey());
			} else {
				ZVAL_LONG(&line, (zend_long) entry.indexKey());
			}

			zv::Val startLine, endLine;
			if (UNEXPECTED(!callMethod(nodeObj, pt_fac_str.get_start_line, 0, NULL, startLine))
				|| UNEXPECTED(!callMethod(nodeObj, pt_fac_str.get_end_line, 0, NULL, endLine))
			) {
				return false;
			}
			if (zend_compare(&line, startLine.raw()) < 0 || zend_compare(&line, endLine.raw()) > 0) {
				continue;
			}

			/* unset($this->unmatchedLineIgnores[$this->file][$line]) */
			zval *unmatchedPerFile = zend_symtable_find(Z_ARRVAL_P(writableArrayProp(PT_FAC_PROP_UNMATCHED_LINE_IGNORES)), file);
			if (unmatchedPerFile == NULL) {
				continue;
			}
			ZVAL_DEREF(unmatchedPerFile);
			if (Z_TYPE_P(unmatchedPerFile) != IS_ARRAY) {
				continue;
			}
			SEPARATE_ARRAY(unmatchedPerFile);
			if (entry.stringKeyOrNull() != NULL) {
				zend_symtable_del(Z_ARRVAL_P(unmatchedPerFile), entry.stringKey());
			} else {
				zend_hash_index_del(Z_ARRVAL_P(unmatchedPerFile), entry.indexKey());
			}
		}

		return true;
	}

	/*
	 * A trait's ignores belong to the file the trait is declared in, which is
	 * not the file being analysed; the union keeps the ones already recorded.
	 */
	bool addTraitLineIgnores(zend_object *nodeObj, zval *scope)
	{
		zv::Val traitNode, fileDescription;
		if (UNEXPECTED(!callMethod(nodeObj, pt_fac_str.get_original_node, 0, NULL, traitNode))
			|| UNEXPECTED(!callMethod(Z_OBJ_P(scope), pt_fac_str.get_file_description, 0, NULL, fileDescription))
		) {
			return false;
		}
		if (UNEXPECTED(!fileDescription.ref().isString())) {
			return typeError("getFileDescription() did not return a string");
		}

		zv::Arr traitNodes = zv::Arr::create(1);
		traitNodes.push(traitNode.ref());
		zv::Val traitLines = getLinesToIgnoreFromTokens(traitNodes.arrRef());
		if (UNEXPECTED(traitLines.isUndef())) {
			return false;
		}

		zval *perFile = dimArray(writableArrayProp(PT_FAC_PROP_LINES_TO_IGNORE), fileDescription.ref().asString());
		if (UNEXPECTED(perFile == NULL)) {
			return false;
		}
		if (traitLines.ref().isArray()) {
			/* $a += $b: the left side wins on a shared key */
			for (auto entry : zv::ArrRef(traitLines.raw())) {
				zval v;
				ZVAL_COPY(&v, entry.value().raw());
				/* the keys come from an array, so a string key is never
				 * numeric and needs no symtable normalisation */
				if (entry.stringKeyOrNull() != NULL) {
					if (zend_hash_add(Z_ARRVAL_P(perFile), entry.stringKey(), &v) == NULL) {
						zval_ptr_dtor(&v);
					}
				} else if (zend_hash_index_add(Z_ARRVAL_P(perFile), entry.indexKey(), &v) == NULL) {
					zval_ptr_dtor(&v);
				}
			}
		}

		zv::Val traitReflection, traitFileName;
		if (UNEXPECTED(!callMethod(nodeObj, pt_fac_str.get_trait_reflection, 0, NULL, traitReflection))) {
			return false;
		}
		if (UNEXPECTED(!traitReflection.ref().isObject())) {
			return typeError("getTraitReflection() did not return an object");
		}
		if (UNEXPECTED(!callMethod(traitReflection.ref().asObject(), pt_fac_str.get_file_name, 0, NULL, traitFileName))) {
			return false;
		}
		if (!traitFileName.ref().isNull()) {
			pushProp(PT_FAC_PROP_PROCESSED_FILES, std::move(traitFileName));
		}

		return true;
	}

	/* foreach ($this->ruleRegistry->getRules($nodeType) as $rule) { ... } */
	bool runRules(zval *node, zval *scope, zval *nodeType, zv::Ref parserNodes, zv::ScratchTable &uniquedMessages)
	{
		zv::Val rules;
		if (UNEXPECTED(!callMethodAt(PT_FAC_SITE_GET_RULES, Z_OBJ_P(prop(PT_FAC_PROP_RULE_REGISTRY).raw()), pt_fac_str.get_rules, 1, nodeType, rules))) {
			return false;
		}
		if (!rules.ref().isArray()) {
			return true;
		}

		zval args[2];
		ZVAL_COPY_VALUE(&args[0], node);
		ZVAL_COPY_VALUE(&args[1], scope);

		for (auto entry : zv::ArrRef(rules.raw())) {
			zv::Ref rule = entry.value().deref();
			if (UNEXPECTED(!rule.isObject())) {
				return typeError("the rule registry returned a non-object");
			}

			zv::Val ruleErrors;
			if (UNEXPECTED(!callProcessNode(rule.asObject(), 2, args, ruleErrors))) {
				bool handled;
				if (UNEXPECTED(!catchReflectionException(node, uniquedMessages, handled)) || !handled) {
					return false;
				}
				continue;
			}
			if (!ruleErrors.ref().isArray() || zend_hash_num_elements(ruleErrors.ref().asArrayTable()) == 0) {
				continue;
			}

			if (UNEXPECTED(!transformRuleErrors(ruleErrors.ref(), node, scope, parserNodes))) {
				return false;
			}
		}

		return true;
	}

	/* the inner foreach of the rule loop: file dependencies, transform, ignores */
	bool transformRuleErrors(zv::Ref ruleErrors, zval *node, zval *scope, zv::Ref parserNodes)
	{
		zend_class_entry *fileDependenciesCe = pt_class(PT_CLASS_FILE_DEPENDENCIES_RULE_ERROR);
		if (UNEXPECTED(fileDependenciesCe == NULL)) {
			return false;
		}

		for (auto entry : zv::ArrRef(ruleErrors.raw())) {
			zv::Ref ruleError = entry.value().deref();
			if (UNEXPECTED(!ruleError.isObject())) {
				return typeError("a rule returned a non-object error");
			}

			if (instanceof_function(ruleError.asObject()->ce, fileDependenciesCe)) {
				/* The rule says its verdict depends on files the dependency
				 * graph cannot know about. */
				zv::Val fileDependencies;
				if (UNEXPECTED(!callMethod(ruleError.asObject(), pt_fac_str.get_file_dependencies, 0, NULL, fileDependencies))) {
					return false;
				}
				pushAllProp(PT_FAC_PROP_FILE_DEPENDENCIES, fileDependencies.ref());
			}

			zval transformArgs[4];
			ZVAL_COPY_VALUE(&transformArgs[0], ruleError.raw());
			ZVAL_COPY_VALUE(&transformArgs[1], scope);
			ZVAL_COPY_VALUE(&transformArgs[2], parserNodes.raw());
			ZVAL_COPY_VALUE(&transformArgs[3], node);

			zv::Val error;
			if (UNEXPECTED(!callMethodAt(PT_FAC_SITE_TRANSFORM, Z_OBJ_P(prop(PT_FAC_PROP_RULE_ERROR_TRANSFORMER).raw()), pt_fac_str.transform, 4, transformArgs, error))) {
				return false;
			}
			if (UNEXPECTED(!error.ref().isObject())) {
				return typeError("the rule error transformer did not return an Error");
			}

			bool ignored;
			if (UNEXPECTED(!shouldIgnoreError(error.ref(), node, scope, ignored))) {
				return false;
			}
			if (ignored) {
				continue;
			}

			pushProp(PT_FAC_PROP_TEMPORARY_FILE_ERRORS, std::move(error));
		}

		return true;
	}

	/* if ($error->canBeIgnored()) { foreach ($this->ignoreErrorExtensions ... ) } */
	bool shouldIgnoreError(zv::Ref error, zval *node, zval *scope, bool &ignored)
	{
		ignored = false;

		zv::Val canBeIgnored;
		if (UNEXPECTED(!callMethodAt(PT_FAC_SITE_CAN_BE_IGNORED, error.asObject(), pt_fac_str.can_be_ignored, 0, NULL, canBeIgnored))) {
			return false;
		}
		if (!canBeIgnored.ref().isTrue()) {
			return true;
		}

		zv::Ref extensions = prop(PT_FAC_PROP_IGNORE_ERROR_EXTENSIONS);
		if (!extensions.isArray()) {
			return true;
		}

		zval args[3];
		ZVAL_COPY_VALUE(&args[0], error.raw());
		ZVAL_COPY_VALUE(&args[1], node);
		ZVAL_COPY_VALUE(&args[2], scope);

		for (auto entry : zv::ArrRef(extensions.raw())) {
			zv::Ref extension = entry.value().deref();
			if (UNEXPECTED(!extension.isObject())) {
				return typeError("an ignore error extension is not an object");
			}
			zv::Val shouldIgnore;
			if (UNEXPECTED(!callMethodAt(PT_FAC_SITE_SHOULD_IGNORE, extension.asObject(), pt_fac_str.should_ignore, 3, args, shouldIgnore))) {
				return false;
			}
			if (shouldIgnore.ref().isTrue()) {
				ignored = true;
				return true;
			}
		}

		return true;
	}

	/* foreach ($this->collectorRegistry->getCollectors($nodeType) as $collector) { ... } */
	bool runCollectors(zval *node, zval *scope, zval *nodeType, zv::ScratchTable &uniquedMessages)
	{
		zv::Val collectors;
		if (UNEXPECTED(!callMethodAt(PT_FAC_SITE_GET_COLLECTORS, Z_OBJ_P(prop(PT_FAC_PROP_COLLECTOR_REGISTRY).raw()), pt_fac_str.get_collectors, 1, nodeType, collectors))) {
			return false;
		}
		if (!collectors.ref().isArray() || zend_hash_num_elements(collectors.ref().asArrayTable()) == 0) {
			return true;
		}

		zval args[2];
		ZVAL_COPY_VALUE(&args[0], node);
		ZVAL_COPY_VALUE(&args[1], scope);

		for (auto entry : zv::ArrRef(collectors.raw())) {
			zv::Ref collector = entry.value().deref();
			if (UNEXPECTED(!collector.isObject())) {
				return typeError("the collector registry returned a non-object");
			}

			zv::Val collectedData;
			if (UNEXPECTED(!callProcessNode(collector.asObject(), 2, args, collectedData))) {
				bool handled;
				if (UNEXPECTED(!catchReflectionException(node, uniquedMessages, handled)) || !handled) {
					return false;
				}
				continue;
			}
			if (collectedData.ref().isNull()) {
				continue;
			}

			zv::Val file;
			if (UNEXPECTED(!callMethodAt(PT_FAC_SITE_GET_FILE, Z_OBJ_P(scope), pt_fac_str.get_file, 0, NULL, file))) {
				return false;
			}
			zval collectorType;
			ZVAL_STR(&collectorType, collector.asObject()->ce->name);
			if (UNEXPECTED(!appendCollectedData(file.ref(), zv::Ref(&collectorType), std::move(collectedData)))) {
				return false;
			}
		}

		return true;
	}

	/*
	 * The dependency block, whose reflection failures are swallowed: a
	 * dependency PHPStan cannot reflect is one the result cache cannot watch,
	 * which is not an error of the analysed file.
	 */
	bool resolveDependencies(zval *node, zval *scope)
	{
		zval args[2];
		ZVAL_COPY_VALUE(&args[0], node);
		ZVAL_COPY_VALUE(&args[1], scope);

		zv::Val dependencies;
		if (UNEXPECTED(!callMethodAt(PT_FAC_SITE_RESOLVE_DEPENDENCIES, Z_OBJ_P(prop(PT_FAC_PROP_DEPENDENCY_RESOLVER).raw()), pt_fac_str.resolve_dependencies, 2, args, dependencies))) {
			return swallowReflectionException();
		}
		if (UNEXPECTED(!dependencies.ref().isObject())) {
			return typeError("resolveDependencies() did not return an object");
		}

		if (UNEXPECTED(!appendDependencies(dependencies.ref().asObject(), scope, PT_FAC_PROP_FILE_DEPENDENCIES))) {
			return swallowReflectionException();
		}

		zv::Val exportedNode;
		if (UNEXPECTED(!callMethodAt(PT_FAC_SITE_GET_EXPORTED_NODE, dependencies.ref().asObject(), pt_fac_str.get_exported_node, 0, NULL, exportedNode))) {
			return swallowReflectionException();
		}
		if (!exportedNode.ref().isNull()) {
			pushProp(PT_FAC_PROP_EXPORTED_NODES, std::move(exportedNode));
		}

		return true;
	}

	/* the InClassNode tail, whose failures do propagate — as in the twin */
	bool resolveUsedTraitDependencies(zval *node, zval *scope)
	{
		zval arg;
		ZVAL_COPY_VALUE(&arg, node);

		zv::Val dependencies;
		if (UNEXPECTED(!callMethodAt(PT_FAC_SITE_RESOLVE_USED_TRAIT_DEPENDENCIES, Z_OBJ_P(prop(PT_FAC_PROP_DEPENDENCY_RESOLVER).raw()), pt_fac_str.resolve_used_trait_dependencies, 1, &arg, dependencies))) {
			return false;
		}
		if (UNEXPECTED(!dependencies.ref().isObject())) {
			return typeError("resolveUsedTraitDependencies() did not return an object");
		}

		return appendDependencies(dependencies.ref().asObject(), scope, PT_FAC_PROP_USED_TRAIT_FILE_DEPENDENCIES);
	}

	/*
	 * The file dependencies of one NodeDependencies into the given slot, and
	 * its package dependencies into filePackageDependencies. resolveDependencies()
	 * additionally takes the paths the node depends on without a symbol, which
	 * only it collects.
	 */
	bool appendDependencies(zend_object *dependencies, zval *scope, uint32_t fileSlot)
	{
		zv::Val file;
		if (UNEXPECTED(!callMethodAt(PT_FAC_SITE_GET_FILE, Z_OBJ_P(scope), pt_fac_str.get_file, 0, NULL, file))) {
			return false;
		}

		zval args[3];
		ZVAL_COPY_VALUE(&args[0], file.raw());
		ZVAL_COPY_VALUE(&args[1], prop(PT_FAC_PROP_ANALYSED_FILES).raw());

		zv::Val fileDependencies;
		if (UNEXPECTED(!callMethodAt(PT_FAC_SITE_GET_FILE_DEPENDENCIES, dependencies, pt_fac_str.get_file_dependencies, 2, args, fileDependencies))) {
			return false;
		}
		pushAllProp(fileSlot, fileDependencies.ref());

		if (fileSlot == PT_FAC_PROP_FILE_DEPENDENCIES) {
			zv::Val filePaths;
			if (UNEXPECTED(!callMethodAt(PT_FAC_SITE_GET_FILE_PATHS, dependencies, pt_fac_str.get_file_paths, 0, NULL, filePaths))) {
				return false;
			}
			pushAllProp(fileSlot, filePaths.ref());
		}

		ZVAL_COPY_VALUE(&args[2], prop(PT_FAC_PROP_PACKAGE_DEPENDENCY_RESOLVER).raw());
		zv::Val nonAnalysed;
		if (UNEXPECTED(!callMethodAt(PT_FAC_SITE_GET_NON_ANALYSED_DEPENDENCIES, dependencies, pt_fac_str.get_non_analysed_dependencies, 3, args, nonAnalysed))) {
			return false;
		}
		if (UNEXPECTED(!nonAnalysed.ref().isArray())) {
			return typeError("getNonAnalysedDependencies() did not return an array");
		}
		pushAllProp(fileSlot, zv::ArrRef(nonAnalysed.raw()).find(pt_fac_str.files_key));
		pushAllProp(PT_FAC_PROP_FILE_PACKAGE_DEPENDENCIES, zv::ArrRef(nonAnalysed.raw()).find(pt_fac_str.packages_key));

		return true;
	}

	/*
	 * catch (AnalysedCodeException | IdentifierNotFound | UnableToCompileNode |
	 * CircularReference) around a rule or collector: the analysed code, not
	 * PHPStan, is broken, so the failure is reported as an error on the node
	 * and the next rule runs. handled = false leaves the exception pending for
	 * the caller to propagate.
	 */
	bool catchReflectionException(zval *node, zv::ScratchTable &uniquedMessages, bool &handled)
	{
		handled = false;
		ZEND_ASSERT(EG(exception) != NULL);

		zend_class_entry *analysedCodeCe = pt_class(PT_CLASS_ANALYSED_CODE_EXCEPTION);
		zend_class_entry *identifierNotFoundCe = pt_class(PT_CLASS_IDENTIFIER_NOT_FOUND);
		zend_class_entry *unableToCompileCe = pt_class(PT_CLASS_UNABLE_TO_COMPILE_NODE);
		zend_class_entry *circularReferenceCe = pt_class(PT_CLASS_CIRCULAR_REFERENCE);
		if (UNEXPECTED(analysedCodeCe == NULL || identifierNotFoundCe == NULL || unableToCompileCe == NULL || circularReferenceCe == NULL)) {
			return false;
		}

		zend_class_entry *thrownCe = EG(exception)->ce;
		bool isAnalysedCode = instanceof_function(thrownCe, analysedCodeCe);
		bool isIdentifierNotFound = !isAnalysedCode && instanceof_function(thrownCe, identifierNotFoundCe);
		bool isUnableToCompile = !isAnalysedCode && !isIdentifierNotFound
			&& (instanceof_function(thrownCe, unableToCompileCe) || instanceof_function(thrownCe, circularReferenceCe));
		if (!isAnalysedCode && !isIdentifierNotFound && !isUnableToCompile) {
			return true;
		}

		zv::Val exception = takeException();
		handled = true;

		if (isAnalysedCode) {
			zv::Val message;
			if (UNEXPECTED(!callMethod(exception.ref().asObject(), pt_fac_str.get_message, 0, NULL, message))) {
				return false;
			}
			if (UNEXPECTED(!message.ref().isString())) {
				return typeError("getMessage() did not return a string");
			}
			/* one error per distinct message per node */
			zval seen;
			ZVAL_NULL(&seen);
			if (zend_hash_add(uniquedMessages.table(), message.ref().asString(), &seen) == NULL) {
				return true;
			}

			zv::Val tip;
			if (UNEXPECTED(!callMethod(exception.ref().asObject(), pt_fac_str.get_tip, 0, NULL, tip))) {
				return false;
			}

			return appendInternalError(message.ref().asString(), node, exception.ref(), tip.ref(), "phpstan.internal", sizeof("phpstan.internal") - 1);
		}

		zv::Str message;
		zval tip;
		ZVAL_NULL(&tip);
		if (isIdentifierNotFound) {
			zv::Val identifier, name;
			if (UNEXPECTED(!callMethod(exception.ref().asObject(), pt_fac_str.get_identifier, 0, NULL, identifier))) {
				return false;
			}
			if (UNEXPECTED(!identifier.ref().isObject())) {
				return typeError("getIdentifier() did not return an object");
			}
			if (UNEXPECTED(!callMethod(identifier.ref().asObject(), pt_fac_str.get_name, 0, NULL, name))) {
				return false;
			}
			if (UNEXPECTED(!name.ref().isString())) {
				return typeError("getName() did not return a string");
			}
			message = zv::Str::adopt(zend_strpprintf(0, "Reflection error: %s not found.", ZSTR_VAL(name.ref().asString())));
			ZVAL_STRINGL(&tip, "Learn more at https://phpstan.org/user-guide/discovering-symbols", sizeof("Learn more at https://phpstan.org/user-guide/discovering-symbols") - 1);
		} else {
			zv::Val exceptionMessage;
			if (UNEXPECTED(!callMethod(exception.ref().asObject(), pt_fac_str.get_message, 0, NULL, exceptionMessage))) {
				return false;
			}
			if (UNEXPECTED(!exceptionMessage.ref().isString())) {
				return typeError("getMessage() did not return a string");
			}
			message = zv::Str::adopt(zend_strpprintf(0, "Reflection error: %s", ZSTR_VAL(exceptionMessage.ref().asString())));
		}

		zv::Val ownedTip = zv::Val::adopt(tip);

		return appendInternalError(message.get(), node, exception.ref(), ownedTip.ref(), "phpstan.reflection", sizeof("phpstan.reflection") - 1);
	}

	/*
	 * catch (AnalysedCodeException | IdentifierNotFound | UnableToCompileNode)
	 * { // pass } around the dependency block. Anything else keeps propagating.
	 */
	bool swallowReflectionException()
	{
		ZEND_ASSERT(EG(exception) != NULL);

		zend_class_entry *analysedCodeCe = pt_class(PT_CLASS_ANALYSED_CODE_EXCEPTION);
		zend_class_entry *identifierNotFoundCe = pt_class(PT_CLASS_IDENTIFIER_NOT_FOUND);
		zend_class_entry *unableToCompileCe = pt_class(PT_CLASS_UNABLE_TO_COMPILE_NODE);
		if (UNEXPECTED(analysedCodeCe == NULL || identifierNotFoundCe == NULL || unableToCompileCe == NULL)) {
			return false;
		}

		zend_class_entry *thrownCe = EG(exception)->ce;
		if (!instanceof_function(thrownCe, analysedCodeCe)
			&& !instanceof_function(thrownCe, identifierNotFoundCe)
			&& !instanceof_function(thrownCe, unableToCompileCe)
		) {
			return false;
		}
		zend_clear_exception();

		return true;
	}

	/* the pending exception, owned by the caller and no longer pending */
	static zv::Val takeException()
	{
		zval exception;
		ZVAL_OBJ_COPY(&exception, EG(exception));
		zend_clear_exception();

		return zv::Val::adopt(exception);
	}

	/*
	 * $this->fileErrors[] = (new Error($message, $this->file, $node->getStartLine(), $e, tip: $tip))
	 *     ->withIdentifier($identifier)
	 *     ->withMetadata([...the exception's trace...]);
	 *
	 * The Error constructor takes filePath and traitFilePath between the
	 * exception and the tip; passing them as null is what the twin's named
	 * argument leaves them at.
	 */
	bool appendInternalError(zend_string *message, zval *node, zv::Ref exception, zv::Ref tip, const char *identifier, size_t identifierLen)
	{
		zend_class_entry *errorCe = pt_class(PT_CLASS_ANALYSER_ERROR);
		if (UNEXPECTED(errorCe == NULL || errorCe->constructor == NULL)) {
			return false;
		}

		zv::Val line;
		if (UNEXPECTED(!callMethod(Z_OBJ_P(node), pt_fac_str.get_start_line, 0, NULL, line))) {
			return false;
		}

		zval errorRaw;
		if (UNEXPECTED(object_init_ex(&errorRaw, errorCe) != SUCCESS)) {
			return false;
		}
		zv::Val error = zv::Val::adopt(errorRaw);

		zval args[7];
		ZVAL_STR(&args[0], message);
		ZVAL_COPY_VALUE(&args[1], prop(PT_FAC_PROP_FILE).raw());
		ZVAL_COPY_VALUE(&args[2], line.raw());
		ZVAL_COPY_VALUE(&args[3], exception.raw());
		ZVAL_NULL(&args[4]);
		ZVAL_NULL(&args[5]);
		ZVAL_COPY_VALUE(&args[6], tip.raw());
		zend_call_known_instance_method(errorCe->constructor, error.ref().asObject(), NULL, 7, args);
		if (UNEXPECTED(EG(exception))) {
			return false;
		}

		zval identifierArg;
		ZVAL_STRINGL(&identifierArg, identifier, identifierLen);
		zv::Val ownedIdentifier = zv::Val::adopt(identifierArg);
		zv::Val identified;
		if (UNEXPECTED(!callMethod(error.ref().asObject(), pt_fac_str.with_identifier, 1, ownedIdentifier.raw(), identified))
			|| UNEXPECTED(!identified.ref().isObject())
		) {
			return false;
		}

		zv::Val metadata = buildTraceMetadata(exception);
		if (UNEXPECTED(metadata.isUndef())) {
			return false;
		}
		zv::Val withMetadata;
		if (UNEXPECTED(!callMethod(identified.ref().asObject(), pt_fac_str.with_metadata, 1, metadata.raw(), withMetadata))) {
			return false;
		}

		pushProp(PT_FAC_PROP_FILE_ERRORS, std::move(withMetadata));

		return true;
	}

	/*
	 * [InternalError::STACK_TRACE_METADATA_KEY => InternalError::prepareTrace($e),
	 *  InternalError::STACK_TRACE_AS_STRING_METADATA_KEY => $e->getTraceAsString()]
	 */
	static zv::Val buildTraceMetadata(zv::Ref exception)
	{
		zend_class_entry *internalErrorCe = pt_class(PT_CLASS_INTERNAL_ERROR);
		if (UNEXPECTED(internalErrorCe == NULL)) {
			return zv::Val();
		}
		zend_function *prepareTrace = (zend_function *) zend_hash_find_ptr(&internalErrorCe->function_table, pt_fac_str.prepare_trace);
		if (UNEXPECTED(prepareTrace == NULL)) {
			zend_throw_error(NULL, "phpstan_turbo: %s::prepareTrace() not found", ZSTR_VAL(internalErrorCe->name));
			return zv::Val();
		}

		zval arg;
		ZVAL_COPY_VALUE(&arg, exception.raw());
		zval traceRaw;
		zend_call_known_function(prepareTrace, NULL, internalErrorCe, &traceRaw, 1, &arg, NULL);
		zv::Val trace = zv::Val::adopt(traceRaw);
		if (UNEXPECTED(EG(exception))) {
			return zv::Val();
		}

		zv::Val traceAsString;
		if (UNEXPECTED(!callMethod(exception.asObject(), pt_fac_str.get_trace_as_string, 0, NULL, traceAsString))) {
			return zv::Val();
		}

		zend_string *traceKey = classConstantString(internalErrorCe, "STACK_TRACE_METADATA_KEY", sizeof("STACK_TRACE_METADATA_KEY") - 1);
		zend_string *traceAsStringKey = classConstantString(internalErrorCe, "STACK_TRACE_AS_STRING_METADATA_KEY", sizeof("STACK_TRACE_AS_STRING_METADATA_KEY") - 1);
		if (UNEXPECTED(traceKey == NULL || traceAsStringKey == NULL)) {
			return zv::Val();
		}

		zv::Arr metadata = zv::Arr::create(2);
		metadata.set(traceKey, std::move(trace));
		metadata.set(traceAsStringKey, std::move(traceAsString));

		return zv::Val(std::move(metadata));
	}

	/* a string class constant of the given class; NULL throws */
	static zend_string *classConstantString(zend_class_entry *ce, const char *name, size_t len)
	{
		zend_class_constant *constant = (zend_class_constant *) zend_hash_str_find_ptr(&ce->constants_table, name, len);
		if (constant != NULL && Z_TYPE(constant->value) == IS_CONSTANT_AST) {
			zval_update_constant_ex(&constant->value, constant->ce);
		}
		if (UNEXPECTED(constant == NULL || Z_TYPE(constant->value) != IS_STRING)) {
			zend_throw_error(NULL, "phpstan_turbo: %s::%s is not a string constant", ZSTR_VAL(ce->name), name);
			return NULL;
		}

		return Z_STR(constant->value);
	}

	/* every false return of this class leaves an exception pending */
	static bool typeError(const char *message)
	{
		zend_type_error("phpstan_turbo: %s", message);
		return false;
	}

	/*
	 * $obj->$method(...$args), with the result owned by the caller. false =
	 * pending exception (or a missing method, which throws an Error).
	 */
	static bool callMethod(zend_object *obj, zend_string *method, uint32_t argc, zval *argv, zv::Val &result)
	{
		return callFunction(resolveMethod(obj->ce, method), obj, method, argc, argv, result);
	}

	/* callMethod() through the given fixed call site's cache */
	static bool callMethodAt(uint32_t site, zend_object *obj, zend_string *method, uint32_t argc, zval *argv, zv::Val &result)
	{
		pt_fac_call_site *cached = &pt_fac_sites[site];
		zend_function *fn;
		if (EXPECTED(cached->ce == obj->ce)) {
			fn = cached->fn;
		} else {
			fn = resolveMethod(obj->ce, method);
			if (EXPECTED(fn != NULL)) {
				cached->ce = obj->ce;
				cached->fn = fn;
			}
		}

		return callFunction(fn, obj, method, argc, argv, result);
	}

	/* callMethod() for processNode(), whose class varies per rule/collector */
	static bool callProcessNode(zend_object *obj, uint32_t argc, zval *argv, zv::Val &result)
	{
		zend_class_entry *ce = obj->ce;
		/* the address is the identity; the low bits are alignment padding */
		pt_fac_call_site *cached = &pt_fac_process_node_cache[(uintptr_t) ce / sizeof(void *) % PT_FAC_PROCESS_NODE_CACHE_LIMIT];
		zend_function *fn;
		if (EXPECTED(cached->ce == ce)) {
			fn = cached->fn;
		} else {
			fn = resolveMethod(ce, pt_fac_str.process_node);
			if (EXPECTED(fn != NULL)) {
				cached->ce = ce;
				cached->fn = fn;
			}
		}

		return callFunction(fn, obj, pt_fac_str.process_node, argc, argv, result);
	}

	static zend_function *resolveMethod(zend_class_entry *ce, zend_string *method)
	{
		return (zend_function *) zend_hash_find_ptr(&ce->function_table, method);
	}

	static bool callFunction(zend_function *fn, zend_object *obj, zend_string *method, uint32_t argc, zval *argv, zv::Val &result)
	{
		if (UNEXPECTED(fn == NULL)) {
			zend_throw_error(NULL, "phpstan_turbo: method %s::%s() not found", ZSTR_VAL(obj->ce->name), ZSTR_VAL(method));
			return false;
		}

		zval retval;
		ZVAL_UNDEF(&retval);
		zend_call_known_instance_method(fn, obj, &retval, argc, argv);
		if (UNEXPECTED(EG(exception))) {
			zval_ptr_dtor(&retval);
			return false;
		}
		result = zv::Val::adopt(retval);

		return true;
	}
};

} // namespace phpstanturbo

using phpstanturbo::FileAnalyserCallback;

/* {{{ engine ABI glue: parameter parsing + registration */

#include "reg.h"

static zend_string *pt_fac_intern(const char *value, size_t len)
{
	return zend_string_init_interned(value, len, 1);
}

#define PT_FAC_INTERN(field, value) pt_fac_str.field = pt_fac_intern(value, sizeof(value) - 1)

void pt_file_analyser_callback_rshutdown()
{
	memset(pt_fac_sites, 0, sizeof(pt_fac_sites));
	memset(pt_fac_process_node_cache, 0, sizeof(pt_fac_process_node_cache));
}

void pt_register_file_analyser_callback()
{
	PT_FAC_INTERN(process_node, "processnode");
	PT_FAC_INTERN(get_rules, "getrules");
	PT_FAC_INTERN(get_collectors, "getcollectors");
	PT_FAC_INTERN(transform, "transform");
	PT_FAC_INTERN(should_ignore, "shouldignore");
	PT_FAC_INTERN(can_be_ignored, "canbeignored");
	PT_FAC_INTERN(resolve_dependencies, "resolvedependencies");
	PT_FAC_INTERN(resolve_used_trait_dependencies, "resolveusedtraitdependencies");
	PT_FAC_INTERN(get_file_dependencies, "getfiledependencies");
	PT_FAC_INTERN(get_file_paths, "getfilepaths");
	PT_FAC_INTERN(get_non_analysed_dependencies, "getnonanalyseddependencies");
	PT_FAC_INTERN(get_exported_node, "getexportednode");
	PT_FAC_INTERN(get_file, "getfile");
	PT_FAC_INTERN(get_file_description, "getfiledescription");
	PT_FAC_INTERN(is_in_trait, "isintrait");
	PT_FAC_INTERN(get_trait_reflection, "gettraitreflection");
	PT_FAC_INTERN(get_file_name, "getfilename");
	PT_FAC_INTERN(parse_file, "parsefile");
	PT_FAC_INTERN(get_start_line, "getstartline");
	PT_FAC_INTERN(get_end_line, "getendline");
	PT_FAC_INTERN(get_original_node, "getoriginalnode");
	PT_FAC_INTERN(get_collector_type, "getcollectortype");
	PT_FAC_INTERN(get_data, "getdata");
	PT_FAC_INTERN(get_message, "getmessage");
	PT_FAC_INTERN(get_tip, "gettip");
	PT_FAC_INTERN(get_identifier, "getidentifier");
	PT_FAC_INTERN(get_name, "getname");
	PT_FAC_INTERN(get_trace_as_string, "gettraceasstring");
	PT_FAC_INTERN(prepare_trace, "preparetrace");
	PT_FAC_INTERN(with_identifier, "withidentifier");
	PT_FAC_INTERN(with_metadata, "withmetadata");
	PT_FAC_INTERN(lines_to_ignore_attribute, "linesToIgnore");
	PT_FAC_INTERN(files_key, "files");
	PT_FAC_INTERN(packages_key, "packages");

	reg::Class cls("PHPStanTurbo\\FileAnalyserCallback");
	/* not final: the stub subclass PHPStan\Analyser\FileAnalyserCallback
	 * extends this class. Declaration order is the OBJ_PROP_NUM order the
	 * PT_FAC_PROP_* slots name — the twelve constructor arguments first, in
	 * the constructor's own order, so construct() can copy them by index. */
	cls.privateNullProperty("file");
	cls.privateArrayProperty("analysedFiles");
	cls.privateNullProperty("ruleRegistry");
	cls.privateNullProperty("collectorRegistry");
	cls.privateNullProperty("outerNodeCallback");
	cls.privateArrayProperty("parserNodes");
	cls.privateArrayProperty("ignoreErrorExtensions");
	cls.privateNullProperty("parser");
	cls.privateNullProperty("dependencyResolver");
	cls.privateNullProperty("packageDependencyResolver");
	cls.privateNullProperty("ruleErrorTransformer");
	cls.privateArrayProperty("processedFiles");
	cls.privateArrayProperty("fileErrors");
	cls.privateArrayProperty("fileCollectedData");
	cls.privateArrayProperty("fileDependencies");
	cls.privateArrayProperty("usedTraitFileDependencies");
	cls.privateArrayProperty("filePackageDependencies");
	cls.privateArrayProperty("exportedNodes");
	cls.privateArrayProperty("temporaryFileErrors");
	cls.privateArrayProperty("linesToIgnore");
	cls.privateArrayProperty("unmatchedLineIgnores");

	cls.method("__construct", reg::Public, 12, {
		reg::stringArg("file"),
		reg::arrayArg("analysedFiles"),
		reg::objectArg("ruleRegistry"),
		reg::objectArg("collectorRegistry"),
		reg::any("outerNodeCallback"),
		reg::arrayArg("parserNodes"),
		reg::arrayArg("ignoreErrorExtensions"),
		reg::objectArg("parser"),
		reg::objectArg("dependencyResolver"),
		reg::objectArg("packageDependencyResolver"),
		reg::objectArg("ruleErrorTransformer"),
		reg::arrayArg("processedFiles"),
	}, [](INTERNAL_FUNCTION_PARAMETERS) {
		zend_string *file;
		zval *analysedFiles, *ruleRegistry, *collectorRegistry, *outerNodeCallback, *parserNodes;
		zval *ignoreErrorExtensions, *parser, *dependencyResolver, *packageDependencyResolver;
		zval *ruleErrorTransformer, *processedFiles;
		ZEND_PARSE_PARAMETERS_START(12, 12)
			Z_PARAM_STR(file)
			Z_PARAM_ARRAY(analysedFiles)
			Z_PARAM_OBJECT(ruleRegistry)
			Z_PARAM_OBJECT(collectorRegistry)
			Z_PARAM_ZVAL(outerNodeCallback)
			Z_PARAM_ARRAY(parserNodes)
			Z_PARAM_ARRAY(ignoreErrorExtensions)
			Z_PARAM_OBJECT(parser)
			Z_PARAM_OBJECT(dependencyResolver)
			Z_PARAM_OBJECT(packageDependencyResolver)
			Z_PARAM_OBJECT(ruleErrorTransformer)
			Z_PARAM_ARRAY(processedFiles)
		ZEND_PARSE_PARAMETERS_END();

		/* the parsed arguments are contiguous on the VM stack in declaration
		 * order, which is the property order construct() copies them into */
		zval args[12];
		ZVAL_STR(&args[0], file);
		ZVAL_COPY_VALUE(&args[1], analysedFiles);
		ZVAL_COPY_VALUE(&args[2], ruleRegistry);
		ZVAL_COPY_VALUE(&args[3], collectorRegistry);
		ZVAL_DEREF(outerNodeCallback);
		ZVAL_COPY_VALUE(&args[4], outerNodeCallback);
		ZVAL_COPY_VALUE(&args[5], parserNodes);
		ZVAL_COPY_VALUE(&args[6], ignoreErrorExtensions);
		ZVAL_COPY_VALUE(&args[7], parser);
		ZVAL_COPY_VALUE(&args[8], dependencyResolver);
		ZVAL_COPY_VALUE(&args[9], packageDependencyResolver);
		ZVAL_COPY_VALUE(&args[10], ruleErrorTransformer);
		ZVAL_COPY_VALUE(&args[11], processedFiles);

		if (UNEXPECTED(!FileAnalyserCallback(ZEND_THIS).construct(args))) {
			RETURN_THROWS();
		}
	});

	cls.method("__invoke", reg::Public, 2, { reg::objectArg("node"), reg::objectArg("scope") }, [](INTERNAL_FUNCTION_PARAMETERS) {
		zval *node, *scope;
		ZEND_PARSE_PARAMETERS_START(2, 2)
			Z_PARAM_OBJECT(node)
			Z_PARAM_OBJECT(scope)
		ZEND_PARSE_PARAMETERS_END();

		if (UNEXPECTED(!FileAnalyserCallback(ZEND_THIS).invoke(node, scope))) {
			RETURN_THROWS();
		}
	});

	cls.method("getFileErrors", reg::Public, 0, {}, [](INTERNAL_FUNCTION_PARAMETERS) {
		ZEND_PARSE_PARAMETERS_NONE();
		FileAnalyserCallback(ZEND_THIS).getFileErrors().intoReturnValue(return_value);
	});

	cls.method("getFileCollectedData", reg::Public, 0, {}, [](INTERNAL_FUNCTION_PARAMETERS) {
		ZEND_PARSE_PARAMETERS_NONE();
		FileAnalyserCallback(ZEND_THIS).getFileCollectedData().intoReturnValue(return_value);
	});

	cls.method("getFileDependencies", reg::Public, 0, {}, [](INTERNAL_FUNCTION_PARAMETERS) {
		ZEND_PARSE_PARAMETERS_NONE();
		FileAnalyserCallback(ZEND_THIS).getFileDependencies().intoReturnValue(return_value);
	});

	cls.method("getPackageDependencies", reg::Public, 0, {}, [](INTERNAL_FUNCTION_PARAMETERS) {
		ZEND_PARSE_PARAMETERS_NONE();
		FileAnalyserCallback(ZEND_THIS).getPackageDependencies().intoReturnValue(return_value);
	});

	cls.method("getUsedTraitFileDependencies", reg::Public, 0, {}, [](INTERNAL_FUNCTION_PARAMETERS) {
		ZEND_PARSE_PARAMETERS_NONE();
		FileAnalyserCallback(ZEND_THIS).getUsedTraitFileDependencies().intoReturnValue(return_value);
	});

	cls.method("getExportedNodes", reg::Public, 0, {}, [](INTERNAL_FUNCTION_PARAMETERS) {
		ZEND_PARSE_PARAMETERS_NONE();
		FileAnalyserCallback(ZEND_THIS).getExportedNodes().intoReturnValue(return_value);
	});

	cls.method("getLinesToIgnore", reg::Public, 0, {}, [](INTERNAL_FUNCTION_PARAMETERS) {
		ZEND_PARSE_PARAMETERS_NONE();
		FileAnalyserCallback(ZEND_THIS).getLinesToIgnore().intoReturnValue(return_value);
	});

	cls.method("getUnmatchedLineIgnores", reg::Public, 0, {}, [](INTERNAL_FUNCTION_PARAMETERS) {
		ZEND_PARSE_PARAMETERS_NONE();
		FileAnalyserCallback(ZEND_THIS).getUnmatchedLineIgnores().intoReturnValue(return_value);
	});

	cls.method("getTemporaryFileErrors", reg::Public, 0, {}, [](INTERNAL_FUNCTION_PARAMETERS) {
		ZEND_PARSE_PARAMETERS_NONE();
		FileAnalyserCallback(ZEND_THIS).getTemporaryFileErrors().intoReturnValue(return_value);
	});

	cls.method("getProcessedFiles", reg::Public, 0, {}, [](INTERNAL_FUNCTION_PARAMETERS) {
		ZEND_PARSE_PARAMETERS_NONE();
		FileAnalyserCallback(ZEND_THIS).getProcessedFiles().intoReturnValue(return_value);
	});

	cls.register_();
}

/* }}} */
