<?php declare(strict_types=1);

// Differential test: native PHPStanTurbo\FileAnalyserCallback vs the PHP twin.
//
// The callback is not a value class — it only makes sense while an analysis
// runs through it — so this drives a real one: a real container, the real
// rules, collectors, dependency resolver and error transformer walk fixture
// files, once with the native callback and once with the twin's own source,
// eval'd under a second name so the reference cannot drift from the file the
// port mirrors. Everything the callback gathers must come out identical:
// errors, collected data, dependencies, exported nodes, line ignores and
// processed files.
//
// Rules and collectors that throw the exceptions the callback catches, an
// error that carries file dependencies, an ignore error extension and an
// outer node callback are injected on top of the real registries — no
// fixture can provoke those branches on its own, and they are where the two
// implementations could most easily disagree.
//
// Run: php -d extension=.../phpstan_turbo.so turbo-ext/tests/file-analyser-callback.php

$root = dirname(__DIR__, 2);
require $root . '/vendor/autoload.php';

if (!extension_loaded('phpstan_turbo')) {
	fwrite(STDERR, "extension not loaded\n");
	exit(2);
}

// Declares the stub subclasses before the autoloader can load the twins.
PHPStan\Turbo\TurboExtensionEnabler::enableIfLoaded();

use PhpParser\Node;
use PHPStan\Analyser\Error;
use PHPStan\Analyser\FileAnalyserCallback;
use PHPStan\Analyser\Scope;
use PHPStan\Analyser\ScopeContext;
use PHPStan\Broker\ClassNotFoundException;
use PHPStan\Collectors\Collector;
use PHPStan\Collectors\RegistryFactory as CollectorRegistryFactory;
use PHPStan\DependencyInjection\ContainerFactory;
use PHPStan\Node\EmitCollectedDataNode;
use PHPStan\Rules\Rule;
use PHPStan\Rules\RuleErrorBuilder;

if (get_parent_class(FileAnalyserCallback::class) !== 'PHPStanTurbo\FileAnalyserCallback') {
	fwrite(STDERR, "the native class is not shadowing the twin — is the extension version current?\n");
	exit(2);
}

$failures = 0;
function check(bool $cond, string $msg): void
{
	global $failures;
	if (!$cond) {
		$failures++;
		echo "FAIL: $msg\n";
	}
}

// The reference is the twin's own source under a second name: the shadowed
// class name is the native one now, and a hand-written copy would drift.
$twinSource = file_get_contents($root . '/src/Analyser/FileAnalyserCallback.php');
$twinBody = substr($twinSource, strpos($twinSource, 'use PhpParser\Node;'));
eval('namespace PHPStan\Analyser { ' . str_replace(
	'final class FileAnalyserCallback',
	'final class ReferenceFileAnalyserCallback',
	$twinBody,
) . ' }');
$referenceClass = 'PHPStan\Analyser\ReferenceFileAnalyserCallback';

// ---- the injected rules, collectors and extensions ----

/** Throws what the callback reports as an internal error, per node. */
final class SmokeThrowingRule implements Rule
{

	public function __construct(private string $className)
	{
	}

	public function getNodeType(): string
	{
		return Node\Stmt\Return_::class;
	}

	public function processNode(Node $node, Scope $scope): array
	{
		throw new ClassNotFoundException($this->className);
	}

}

/** Throws what the callback reports as a reflection error, per node. */
final class SmokeReflectionThrowingRule implements Rule
{

	public function __construct(private string $kind)
	{
	}

	public function getNodeType(): string
	{
		return Node\Stmt\Return_::class;
	}

	public function processNode(Node $node, Scope $scope): array
	{
		if ($this->kind === 'identifier') {
			throw new PHPStan\BetterReflection\Reflector\Exception\IdentifierNotFound(
				'Smoke identifier not found',
				new PHPStan\BetterReflection\Identifier\Identifier(
					'SmokeMissingClass',
					new PHPStan\BetterReflection\Identifier\IdentifierType(PHPStan\BetterReflection\Identifier\IdentifierType::IDENTIFIER_CLASS),
				),
			);
		}
		if ($this->kind === 'compile') {
			throw new PHPStan\BetterReflection\NodeCompiler\Exception\UnableToCompileNode('Smoke unable to compile');
		}

		throw PHPStan\BetterReflection\Reflection\Exception\CircularReference::fromClassName('SmokeCircular');
	}

}

/** An error whose verdict depends on files outside the dependency graph. */
final class SmokeFileDependenciesRule implements Rule
{

	public function __construct(private string $file)
	{
	}

	public function getNodeType(): string
	{
		return Node\Stmt\Return_::class;
	}

	public function processNode(Node $node, Scope $scope): array
	{
		return [
			RuleErrorBuilder::message('Smoke error with a file dependency')
				->identifier('smoke.fileDependency')
				->fileDependency($this->file)
				->build(),
		];
	}

}

/** One error the ignore extension below drops, one it keeps. */
final class SmokeIgnorableRule implements Rule
{

	public function getNodeType(): string
	{
		return Node\Stmt\Return_::class;
	}

	public function processNode(Node $node, Scope $scope): array
	{
		return [
			RuleErrorBuilder::message('Smoke ignored error')->identifier('smoke.ignored')->build(),
			RuleErrorBuilder::message('Smoke kept error')->identifier('smoke.kept')->build(),
		];
	}

}

final class SmokeIgnoreErrorExtension implements PHPStan\Analyser\IgnoreErrorExtension
{

	public function shouldIgnore(Error $error, Node $node, Scope $scope): bool
	{
		return $error->getMessage() === 'Smoke ignored error';
	}

}

/** @implements Collector<Node\Stmt\Return_, string> */
final class SmokeCollector implements Collector
{

	public function getNodeType(): string
	{
		return Node\Stmt\Return_::class;
	}

	public function processNode(Node $node, Scope $scope)
	{
		// null is the collector's way of collecting nothing; both branches run
		return $node->getStartLine() % 2 === 0 ? null : 'collected at ' . $node->getStartLine();
	}

}

/** @implements Collector<Node\Stmt\Return_, string> */
final class SmokeThrowingCollector implements Collector
{

	public function __construct(private string $className)
	{
	}

	public function getNodeType(): string
	{
		return Node\Stmt\Return_::class;
	}

	public function processNode(Node $node, Scope $scope)
	{
		throw new ClassNotFoundException($this->className);
	}

}

// ---- the container and the services the callback is built from ----

$tmpDir = sys_get_temp_dir() . '/phpstan-turbo-file-analyser-callback';
PHPStan\Internal\DirectoryCreator::ensureDirectoryExists($tmpDir, 0777);
$fixtureDir = __DIR__ . '/file-analyser-callback-fixtures';
$containerFactory = new ContainerFactory($root);
// the fixtures are an analysed path, so their symbols are discovered: without
// reflection for them there is no trait analysis and no file dependencies
$container = $containerFactory->create($tmpDir, [$containerFactory->getConfigDirectory() . '/config.level8.neon'], [$fixtureDir]);
foreach ($container->getParameter('bootstrapFiles') as $bootstrapFile) {
	(static function (string $file): void {
		require_once $file;
	})($bootstrapFile);
}

$parser = $container->getService('defaultAnalysisParser');
$scopeFactory = $container->getByType(PHPStan\Analyser\ScopeFactory::class);
$nodeScopeResolver = $container->getByType(PHPStan\Analyser\NodeScopeResolver::class);
$dependencyResolver = $container->getByType(PHPStan\Dependency\DependencyResolver::class);
$packageDependencyResolver = $container->getByType(PHPStan\Dependency\PackageDependencyResolver::class);
$ruleErrorTransformer = $container->getByType(PHPStan\Analyser\RuleErrorTransformer::class);
$realRuleRegistry = $container->getByType(PHPStan\Rules\Registry::class);

$fixtures = [
	'class using a trait' => $fixtureDir . '/FixtureClass.php',
	'trait declaration' => $fixtureDir . '/FixtureTrait.php',
	'plain class' => $fixtureDir . '/FixtureDependency.php',
];
$analysedFiles = [];
foreach ($fixtures as $fixtureFile) {
	$analysedFiles[$fixtureFile] = true;
}
// without this the path routing parser hands out bodyless nodes for files
// outside the analysed paths, and no rule ever sees a statement
$container->getService('pathRoutingParser')->setAnalysedFiles(array_keys($analysedFiles));
$nodeScopeResolver->setAnalysedFiles(array_keys($analysedFiles));

// The rules the fixtures cannot provoke, on top of the real ones. Two
// throwers share a message so that the per-node deduplication of
// AnalysedCodeException messages is exercised — including across the rule
// and the collector loop, which share one table.
$extraRules = [
	new SmokeThrowingRule('SmokeDuplicate'),
	new SmokeThrowingRule('SmokeDuplicate'),
	new SmokeThrowingRule('SmokeOther'),
	new SmokeReflectionThrowingRule('identifier'),
	new SmokeReflectionThrowingRule('compile'),
	new SmokeReflectionThrowingRule('circular'),
	new SmokeFileDependenciesRule($fixtureDir . '/not-a-symbol.json'),
	new SmokeIgnorableRule(),
];
$ruleRegistry = new class ($realRuleRegistry, $extraRules) implements PHPStan\Rules\Registry {

	/** @param Rule[] $extraRules */
	public function __construct(private PHPStan\Rules\Registry $inner, private array $extraRules)
	{
	}

	public function getRules(string $nodeType): array
	{
		$rules = $this->inner->getRules($nodeType);
		foreach ($this->extraRules as $rule) {
			if ($rule->getNodeType() !== $nodeType) {
				continue;
			}
			$rules[] = $rule;
		}

		return $rules;
	}

};

$collectorRegistry = new PHPStan\Collectors\Registry(array_merge(
	array_values($container->getServicesByTag(CollectorRegistryFactory::COLLECTOR_TAG)),
	[new SmokeCollector(), new SmokeThrowingCollector('SmokeDuplicate')],
));

$ignoreErrorExtensions = [new SmokeIgnoreErrorExtension()];

/**
 * Everything the callback gathered, as comparable data. The stack traces in
 * the internal errors' metadata are the one thing that legitimately differs:
 * the frames below the throw site are the callback itself, which is a PHP
 * frame on one side and an internal one on the other. The throw site — the
 * first prepared frame — is compared, the rest is reduced to its shape.
 */
$describe = static function (array $result): array {
	$describeValue = static function ($value) use (&$describeValue) {
		if (is_array($value)) {
			return array_map($describeValue, $value);
		}
		if ($value instanceof JsonSerializable) {
			return $describeValue($value->jsonSerialize());
		}
		if (is_object($value)) {
			return get_class($value) . ': ' . print_r(get_object_vars($value), true);
		}

		return $value;
	};

	$describeErrors = static function (array $errors) use ($describeValue): array {
		return array_map(static function (Error $error) use ($describeValue): array {
			$data = $describeValue($error->jsonSerialize());
			if (isset($data['metadata']['stackTrace'])) {
				$data['metadata']['stackTrace'] = [
					'throwSite' => $data['metadata']['stackTrace'][0],
					'frames' => count($data['metadata']['stackTrace']) > 1,
				];
			}
			if (isset($data['metadata']['stackTraceAsString'])) {
				$data['metadata']['stackTraceAsString'] = is_string($data['metadata']['stackTraceAsString']) && $data['metadata']['stackTraceAsString'] !== '';
			}

			return $data;
		}, $errors);
	};

	$result['fileErrors'] = $describeErrors($result['fileErrors']);
	$result['temporaryFileErrors'] = $describeErrors($result['temporaryFileErrors']);
	$result['fileCollectedData'] = $describeValue($result['fileCollectedData']);
	$result['exportedNodes'] = $describeValue($result['exportedNodes']);

	return $result;
};

/** Runs one fixture file through one implementation of the callback. */
$run = static function (string $class, string $file) use (
	$parser,
	$scopeFactory,
	$nodeScopeResolver,
	$dependencyResolver,
	$packageDependencyResolver,
	$ruleErrorTransformer,
	$ruleRegistry,
	$collectorRegistry,
	$ignoreErrorExtensions,
	$analysedFiles
): array {
	$parserNodes = $parser->parseFile($file);
	$outerNodes = [];
	$callback = new $class(
		$file,
		$analysedFiles,
		$ruleRegistry,
		$collectorRegistry,
		static function (Node $node, Scope $scope) use (&$outerNodes): void {
			$outerNodes[] = get_class($node);
		},
		$parserNodes,
		$ignoreErrorExtensions,
		$parser,
		$dependencyResolver,
		$packageDependencyResolver,
		$ruleErrorTransformer,
		[$file],
	);

	$scope = $scopeFactory->create(ScopeContext::create($file), $callback);
	$callback(new PHPStan\Node\FileNode($parserNodes), $scope);
	$nodeScopeResolver->resetPerFileAnalysisState();
	$nodeScopeResolver->processNodes($parserNodes, $scope, $callback);

	// the collected-data branch: emitted by MutatingScope during an analysis,
	// which no fixture reaches from a rule, so it is driven directly here
	$callback(new EmitCollectedDataNode(SmokeCollector::class, ['emitted', $file]), $scope);

	return [
		'fileErrors' => $callback->getFileErrors(),
		'temporaryFileErrors' => $callback->getTemporaryFileErrors(),
		'fileCollectedData' => $callback->getFileCollectedData(),
		'fileDependencies' => $callback->getFileDependencies(),
		'usedTraitFileDependencies' => $callback->getUsedTraitFileDependencies(),
		'packageDependencies' => $callback->getPackageDependencies(),
		'exportedNodes' => $callback->getExportedNodes(),
		'linesToIgnore' => $callback->getLinesToIgnore(),
		'unmatchedLineIgnores' => $callback->getUnmatchedLineIgnores(),
		'processedFiles' => $callback->getProcessedFiles(),
		'outerNodes' => $outerNodes,
	];
};

foreach ($fixtures as $label => $fixtureFile) {
	$reference = $describe($run($referenceClass, $fixtureFile));
	$referenceAgain = $describe($run($referenceClass, $fixtureFile));
	$native = $describe($run(FileAnalyserCallback::class, $fixtureFile));

	// a second reference run pins that the comparison below is about the two
	// implementations and not about analysis state carried between runs
	check($reference === $referenceAgain, "$label: the PHP implementation is stable across runs");
	foreach ($reference as $key => $expected) {
		check($expected === $native[$key], sprintf(
			"%s: %s differs\n  php:    %s\n  native: %s",
			$label,
			$key,
			var_export($expected, true),
			var_export($native[$key] ?? null, true),
		));
	}
	// What the fixtures are here to exercise — the equality above only means
	// something for the branches a run actually reaches. Every injected rule
	// fires on return statements, so their error counts are pinned to how many
	// the file has; a file without any (the trait declaration) only exercises
	// the ignore bookkeeping.
	$returnCount = count(array_keys($reference['outerNodes'], Node\Stmt\Return_::class, true));
	check(count($reference['exportedNodes']) > 0, "$label: the file exports a node");
	check(count($reference['fileCollectedData']) > 0, "$label: the file collects data");

	$messages = array_column($reference['temporaryFileErrors'], 'message');
	$internalMessages = array_column($reference['fileErrors'], 'message');
	$countReported = static fn (string $needle): int => count(array_filter(
		$internalMessages,
		static fn (string $message): bool => str_contains($message, $needle),
	));
	check(count($messages) === count(array_unique(array_keys($messages))), "$label: rule errors are collected in order");
	check(!in_array('Smoke ignored error', $messages, true), "$label: an error the ignore extension drops is gone");
	check(
		count(array_keys($messages, 'Smoke kept error', true)) === $returnCount,
		"$label: an error no extension ignores is kept",
	);
	check(
		count(array_keys($messages, 'Smoke error with a file dependency', true)) === $returnCount,
		"$label: the file-dependencies error is reported",
	);
	check(
		($returnCount > 0) === in_array($fixtureDir . '/not-a-symbol.json', $reference['fileDependencies'], true),
		"$label: the error's file dependency is recorded",
	);
	// the same message from two rules and the throwing collector is reported once per node
	check($countReported('SmokeDuplicate') === $returnCount, "$label: the duplicated exception message is reported once per node");
	// the reflection errors carry the identifier's name, not the exception message
	foreach (['SmokeOther', 'SmokeMissingClass', 'Smoke unable to compile', 'Circular reference'] as $expected) {
		check($countReported($expected) === $returnCount, "$label: the \"$expected\" exception is reported per node");
	}
}

// An exception the callback does not catch propagates out of it on both sides.
$uncaughtRule = new class implements Rule {

	public function getNodeType(): string
	{
		return Node\Stmt\Return_::class;
	}

	public function processNode(Node $node, Scope $scope): array
	{
		throw new RuntimeException('Smoke uncaught');
	}

};
$uncaughtRegistry = new class ($uncaughtRule) implements PHPStan\Rules\Registry {

	public function __construct(private Rule $rule)
	{
	}

	public function getRules(string $nodeType): array
	{
		return $nodeType === $this->rule->getNodeType() ? [$this->rule] : [];
	}

};
$uncaught = [];
foreach (['php' => $referenceClass, 'native' => FileAnalyserCallback::class] as $side => $class) {
	$file = $fixtures['plain class'];
	$parserNodes = $parser->parseFile($file);
	$callback = new $class(
		$file,
		$analysedFiles,
		$uncaughtRegistry,
		$collectorRegistry,
		null,
		$parserNodes,
		[],
		$parser,
		$dependencyResolver,
		$packageDependencyResolver,
		$ruleErrorTransformer,
		[$file],
	);
	$scope = $scopeFactory->create(ScopeContext::create($file), $callback);
	try {
		$nodeScopeResolver->resetPerFileAnalysisState();
		$nodeScopeResolver->processNodes($parserNodes, $scope, $callback);
		$uncaught[$side] = 'nothing thrown';
	} catch (Throwable $e) {
		$uncaught[$side] = get_class($e) . ': ' . $e->getMessage();
	}
}
check($uncaught['php'] === $uncaught['native'], 'an exception the callback does not catch propagates identically: ' . json_encode($uncaught));
check($uncaught['native'] === 'RuntimeException: Smoke uncaught', 'the uncaught exception is the rule\'s own');

echo $failures === 0 ? "ALL OK\n" : "$failures FAILURES\n";
exit($failures === 0 ? 0 : 1);
