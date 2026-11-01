<?php

namespace ConstantConditionFunctionCall;

function retObj(): object
{
	return new \stdClass();
}

function doFoo(int $int): void
{
	// reported by the ImpossibleCheckType rule, NOT by the constant-condition rule
	if (is_int($int)) {
		echo 'always';
	}

	// reported by the constant-condition rule: an always-truthy return
	// that is not a type-check
	if (retObj()) {
		echo 'always';
	}

	// object can be a falsy GMP, SimpleXMLElement or BcMath\Number,
	// but stdClass is always truthy
	if (retStdClass()) {
		echo 'always';
	}
}

function retStdClass(): \stdClass
{
	return new \stdClass();
}
