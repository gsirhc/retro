for (let num = 2 ; num < 65535; num++) {
    let isPrime = true;

    // looping through 2 to number-1
    for (let i = 2; i < num; i++) {
        if (num % i == 0) {
            isPrime = false;
            break;
        }
    }

    if (isPrime) {
        process.stdout.write(`${num} `);
    }
}