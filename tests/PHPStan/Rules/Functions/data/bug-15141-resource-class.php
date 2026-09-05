<?php

// The functions are not called doFoo()/doBar() on purpose: the class has to stay
// in the global namespace, and global function names leak into other tests.

class Resource {}

function bug15141TakesResourceClass(Resource $r):void {}

$r = new Resource();
bug15141TakesResourceClass($r);

/** @param mixed $m */
function bug15141TakesMixed($m):void {
    if (is_resource($m)) {
        bug15141TakesResourceClass($m);
    }
}
