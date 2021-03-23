
void *worker_slow(void *task)
{
	union pubonionunion pubonion;
	u8 * const pk = &pubonion.raw[PKPREFIX_SIZE];
	u8 secret[SKPREFIX_SIZE + SECRET_LEN];
	u8 * const sk = &secret[SKPREFIX_SIZE];
	u8 seed[SEED_LEN];
	u8 hashsrc[checksumstrlen + PUBLIC_LEN + 1];
	u8 wpk[PUBLIC_LEN + 1];
	char *sname;

	size_t i;

#ifdef STATISTICS
	struct statstruct *st = (struct statstruct *)task;
#else
	(void) task;
#endif
	PREFILTER

	memcpy(secret,skprefix,SKPREFIX_SIZE);
	wpk[PUBLIC_LEN] = 0;
	memset(&pubonion,0,sizeof(pubonion));
	memcpy(pubonion.raw,pkprefix,PKPREFIX_SIZE);
	// write version later as it will be overwritten by hash
	memcpy(hashsrc,checksumstr,checksumstrlen);0
	hashsrc[checksumstrlen + PUBLIC_LEN] = 0x03; // version

///
	/* Don't use a seed anymore */

	/*randombytes(seed,sizeof(seed));*/
///
	ed25519_seckey_expand(sk,NULL);

	ed25519_pubkey(pk,sk);

///
	/* Removed the sanity check, not neccessary */
/*	// sanity check
	if ((sk[0] & 248) != sk[0] || ((sk[31] & 63) | 64) != sk[31])
		goto initseed; */
///

	// calc checksum
	memcpy(&hashsrc[checksumstrlen],pk,PUBLIC_LEN);
	FIPS202_SHA3_256(hashsrc,sizeof(hashsrc),&pk[PUBLIC_LEN]);
	// version byte
	pk[PUBLIC_LEN + 2] = 0x03;
	// base32
	strcpy(base32_to(&sname[direndpos],pk,PUBONION_LEN),".onion");

///
	/* Prints the hash */
	printf("%s\n", &sname[direndpos]);
///

	pk[PUBLIC_LEN] = 0; // what is this for?

///
	// Stops everything
	endwork = 1;
///

	POSTFILTER
	sodium_memzero(secret,sizeof(secret));
	sodium_memzero(seed,sizeof(seed));
	return 0;
}
