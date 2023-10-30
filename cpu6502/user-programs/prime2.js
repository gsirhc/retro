for (let n = 2 ; n < 1000; n++) {
    let isPrime = true;

    if (n <= 1 || n % 2 == 0 || n % 3 == 0)
        isPrime = false;
    else {
        const limit = Math.sqrt(n)
        for (let i = 5; i <= limit+1; i += 6)
        {
            if (n % i == 0 || n % (i + 2) == 0)
                isPrime = false;
        }
    }

    if (isPrime) {
        process.stdout.write(`${n} `);
    }
}