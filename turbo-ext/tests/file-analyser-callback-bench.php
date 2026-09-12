<?php declare(strict_types=1);

// Benchmark: the per-node cost of PHPStanTurbo\FileAnalyserCallback against
// the PHP twin, on the same node, scope and services.
//
// What a port of this class can win is the bookkeeping around the calls it
// makes, not the calls themselves: the rules, collectors and the dependency
// resolver run as PHP either way. So the rules and collectors here are
// no-ops — their own bodies would only add the same constant to both sides
// and drown the difference — while their number matches what a real run
// sees (a self-analysis averages ~24 rule and collector calls per node).
//
// The per-call difference times the number of nodes a run visits is the
// port's effect; tests/file-analyser-callback.php is what says the two sides
// do the same thing.
//
// Run: php -d extension=.../phpstan_turbo.so turbo-ext/tests/file-analyser-callback-bench.php

$root = dirname(__DIR__, 2);
require $root . '/vendor/autoload.php';

if (!extension_loaded('phpstan_turbo')) {
	fwrite(STDERR, "extension not loaded\n");
	exit(2);
}

PHPStan\Turbo\TurboExtensionEnabler::enableIfLoaded();

use PhpParser\Node;
use PHPStan\Analyser\FileAnalyserCallback;
use PHPStan\Analyser\Scope;
use PHPStan\Analyser\ScopeContext;
use PHPStan\Collectors\Collector;
use PHPStan\DependencyInjection\ContainerFactory;
use PHPStan\Rules\Rule;

if (get_parent_class(FileAnalyserCallback::class) !== 'PHPStanTurbo\FileAnalyserCallback') {
	fwrite(STDERR, "the native class is not shadowing the twin — is the extension version current?\n");
	exit(2);
}

// the reference is the twin's own source under a second name (see
// tests/file-analyser-callback.php)
$twinSource = file_get_contents($root . '/src/Analyser/FileAnalyserCallback.php');
$twinBody = substr($twinSource, strpos($twinSource, 'use PhpParser\Node;'));
eval('namespace PHPStan\Analyser { ' . str_replace(
	'final class FileAnalyserCallback',
	'final class BenchFileAnalyserCallback',
	$twinBody,
) . ' }');

/** @implements Rule<Node> */
final class BenchNoopRule implements Rule
{

	public function getNodeType(): string
	{
		return Node::class;
	}

	public function processNode(Node $node, Scope $scope): array
	{
		return [];
	}

}

/** @implements Collector<Node, null> */
final class BenchNoopCollector implements Collector
{

	public function getNodeType(): string
	{
		return Node::class;
	}

	public function processNode(Node $node, Scope $scope)
	{
		return null;
	}

}

$tmpDir = sys_get_temp_dir() . '/phpstan-turbo-file-analyser-callback-bench';
PHPStan\Internal\DirectoryCreator::ensureDirectoryExists($tmpDir, 0777);
$fixtureDir = __DIR__ . '/file-analyser-callback-fixtures';
$containerFactory = new ContainerFactory($root);
$container = $containerFactory->create($tmpDir, [$containerFactory->getConfigDirectory() . '/config.level8.neon'], [$fixtureDir]);
foreach ($container->getParameter('bootstrapFiles') as $bootstrapFile) {
	(static function (string $file): void {
		require_once $file;
	})($bootstrapFile);
}

$file = $fixtureDir . '/FixtureDependency.php';
$parser = $container->getService('defaultAnalysisParser');
$container->getService('pathRoutingParser')->setAnalysedFiles([$file]);
$nodeScopeResolver = $container->getByType(PHPStan\Analyser\NodeScopeResolver::class);
$nodeScopeResolver->setAnalysedFiles([$file]);
$scopeFactory = $container->getByType(PHPStan\Analyser\ScopeFactory::class);
$parserNodes = $parser->parseFile($file);

// a real node and the scope it was visited in, so the dependency resolver
// and the scope calls do what they do in an analysis
$benchNode = null;
$benchScope = null;
$capture = static function (Node $node, Scope $scope) use (&$benchNode, &$benchScope): void {
	if ($benchNode !== null || !$node instanceof Node\Scalar\Int_) {
		return;
	}
	$benchNode = $node;
	$benchScope = $scope;
};
$nodeScopeResolver->resetPerFileAnalysisState();
$nodeScopeResolver->processNodes($parserNodes, $scopeFactory->create(ScopeContext::create($file), $capture), $capture);
if ($benchNode === null) {
	fwrite(STDERR, "no node captured\n");
	exit(2);
}

$ruleCount = (int) ($argv[1] ?? 22);
$collectorCount = (int) ($argv[2] ?? 2);
$iterations = (int) ($argv[3] ?? 200000);
$rounds = (int) ($argv[4] ?? 5);

$rules = [];
for ($i = 0; $i < $ruleCount; $i++) {
	$rules[] = new BenchNoopRule();
}
$collectors = [];
for ($i = 0; $i < $collectorCount; $i++) {
	$collectors[] = new BenchNoopCollector();
}
$ruleRegistry = new class ($rules) implements PHPStan\Rules\Registry {

	/** @param Rule[] $rules */
	public function __construct(private array $rules)
	{
	}

	public function getRules(string $nodeType): array
	{
		return $this->rules;
	}

};
$collectorRegistry = new PHPStan\Collectors\Registry($collectors);

$make = static function (string $class) use ($file, $parserNodes, $ruleRegistry, $collectorRegistry, $parser, $container) {
	return new $class(
		$file,
		[$file => true],
		$ruleRegistry,
		$collectorRegistry,
		null,
		$parserNodes,
		[],
		$parser,
		$container->getByType(PHPStan\Dependency\DependencyResolver::class),
		$container->getByType(PHPStan\Dependency\PackageDependencyResolver::class),
		$container->getByType(PHPStan\Analyser\RuleErrorTransformer::class),
		[$file],
	);
};

// the arrays the callback appends to are rebuilt every chunk, so neither side
// pays for a table that grew over the whole measurement
gc_disable();
$chunk = 2000;
$measure = static function (string $class) use ($make, $benchNode, $benchScope, $iterations, $chunk): float {
	$elapsed = 0.0;
	for ($done = 0; $done < $iterations; $done += $chunk) {
		$callback = $make($class);
		$start = hrtime(true);
		for ($i = 0; $i < $chunk; $i++) {
			$callback($benchNode, $benchScope);
		}
		$elapsed += (hrtime(true) - $start) / 1e9;
	}

	return $elapsed;
};

$sides = [
	'php' => 'PHPStan\Analyser\BenchFileAnalyserCallback',
	'native' => FileAnalyserCallback::class,
];

// warm up both sides before timing either
foreach ($sides as $class) {
	$measure($class);
}

$best = [];
for ($round = 0; $round < $rounds; $round++) {
	// interleaved, so a drifting machine cannot favour one side
	foreach ($sides as $side => $class) {
		$elapsed = $measure($class);
		$best[$side] = isset($best[$side]) ? min($best[$side], $elapsed) : $elapsed;
		printf("round %d %-6s %.3fs (%.0f ns/call)\n", $round + 1, $side, $elapsed, $elapsed / $iterations * 1e9);
	}
}

$phpPerCall = $best['php'] / $iterations * 1e9;
$nativePerCall = $best['native'] / $iterations * 1e9;
printf(
	"\nbest of %d rounds, %d rules + %d collectors per node:\n  php    %.0f ns/call\n  native %.0f ns/call\n  saved  %.0f ns/call (%.1f%%)\n",
	$rounds,
	$ruleCount,
	$collectorCount,
	$phpPerCall,
	$nativePerCall,
	$phpPerCall - $nativePerCall,
	($phpPerCall - $nativePerCall) / $phpPerCall * 100,
);
