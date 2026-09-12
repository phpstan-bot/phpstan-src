<?php declare(strict_types = 1);

namespace PHPStan;

use Exception;
use PHPStan\Turbo\ReferencedByTurboExtension;

#[ReferencedByTurboExtension(key: 'analysedCodeException')]
abstract class AnalysedCodeException extends Exception
{

	abstract public function getTip(): ?string;

}
