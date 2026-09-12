<?php declare(strict_types = 1);

namespace TurboFixtures;

final class FixtureClass
{

	use FixtureTrait;

	private int $unused = 1;

	public function wrongReturn(): string
	{
		return 1;
	}

	public function ignoredReturn(): string
	{
		return 1; // @phpstan-ignore return.type (the matched ignore)
	}

	public function unmatchedIgnore(): string
	{
		// @phpstan-ignore return.type (nothing to ignore on the next line)
		return 'fine';
	}

	public function dependency(): FixtureDependency
	{
		return new FixtureDependency();
	}

}
