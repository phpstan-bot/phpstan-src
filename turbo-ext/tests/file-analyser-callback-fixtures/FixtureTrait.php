<?php declare(strict_types = 1);

namespace TurboFixtures;

trait FixtureTrait
{

	public function traitMethod(): int
	{
		return 'not an int';
	}

	public function traitIgnored(): int
	{
		return 'not an int either'; // @phpstan-ignore return.type (the ignore matched inside the trait)
	}

}
