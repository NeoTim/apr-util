/* Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "apr.h"
#include "apr_errno.h"
#include "apr_pools.h"
#include "apr_strings.h"
#define APR_WANT_MEMFUNC
#define APR_WANT_STRFUNC
#include "apr_want.h"
#include "apr_general.h"

#include "apu_config.h"

#ifdef APU_HAVE_OPENSSL

#include "apu.h"
#include "apr_portable.h"


#include "apr_ssl.h"
#include "apr_ssl_private.h"
#include "apr_ssl_openssl_private.h"

apr_status_t apu_ssl_init(void)
{
    CRYPTO_malloc_init();
    SSL_load_error_strings();
    SSL_library_init();
    OpenSSL_add_all_algorithms();
    return APR_SUCCESS;
}

/* SSL_get_error() docs say that this MUST be called in the same
 * thread as the operation that failed, and that no other
 * SSL_ operations should be called between the error being reported
 * and the call to get the error code made, hence this function should
 * be called within the function that generates the error.
 * TODO - this should be expanded to generate the correct APR_ errors
 *        when we have created the mappings :-)
 */
static void openssl_get_error(apr_ssl_socket_t *sock, int fncode)
{
    sock->sslData->err = fncode;
    sock->sslData->sslErr = SSL_get_error(sock->sslData->ssl, fncode);
}

/* The apr_ssl_factory_t structure will have the pool and purpose
 * fields set only.
 */
apr_status_t apu_ssl_factory_create(apr_ssl_factory_t *asf,
                                 const char *privateKeyFn,
                                 const char *certFn,
                                 const char *digestType)
{
    apu_ssl_data_t *sslData = apr_pcalloc(asf->pool, sizeof(*sslData));
    if (!sslData) {
        return APR_ENOMEM;
    }

    if (asf->purpose == APR_SSL_FACTORY_SERVER) {
        sslData->ctx = SSL_CTX_new(SSLv23_server_method());
        if (sslData->ctx) {
            if (!SSL_CTX_use_PrivateKey_file(sslData->ctx, privateKeyFn,
                                             SSL_FILETYPE_PEM) ||
                !SSL_CTX_use_certificate_file(sslData->ctx, certFn, 
                                              SSL_FILETYPE_PEM) ||
                !SSL_CTX_check_private_key(sslData->ctx)) {
                SSL_CTX_free(sslData->ctx);
                return APR_ENOENT; /* what code shoudl we return? */
            }
        }
    } else {
        sslData->ctx = SSL_CTX_new(SSLv23_client_method());
    }   

    if (digestType) {
        sslData->md = EVP_get_digestbyname(digestType);
        /* we don't care if this fails... */
    }

    if (!sslData->ctx)
        return APR_EGENERAL; /* what error code? */

    asf->sslData = sslData;

    return APR_SUCCESS;
}

apr_status_t apu_ssl_socket_create(apr_ssl_socket_t *sslSock, 
                                   apr_ssl_factory_t *asf)
{
    apu_ssl_socket_data_t *sslData = apr_pcalloc(sslSock->pool, 
                                                 sizeof(*sslData));
    apr_os_sock_t fd;

    if (!sslData || !asf->sslData)
        return APR_EINVAL;
    sslData->ssl = SSL_new(asf->sslData->ctx);
    if (!sslData->ssl)
        return APR_EINVALSOCK; /* Hmm, better error code? */

    /* Joe Orton points out this is actually wrong and assumes that
     * that we're on an "fd" system. We need some better way of handling
     * this for systems that don't use fd's for sockets. Will?
     */
    if (apr_os_sock_get(&fd, sslSock->plain) != APR_SUCCESS)
        return APR_EINVALSOCK;

    SSL_set_fd(sslData->ssl, fd);
    sslSock->sslData = sslData;
    return APR_SUCCESS;
}

apr_status_t apu_ssl_socket_close(apr_ssl_socket_t *sock)
{
    int sslRv;
    apr_status_t rv;

    if (!sock->sslData->ssl)
        return APR_SUCCESS;

    if (sock->connected) {
        if ((sslRv = SSL_shutdown(sock->sslData->ssl)) == 0)
            sslRv = SSL_shutdown(sock->sslData->ssl);
        if (sslRv == -1)
            return APR_EINVALSOCK; /* Better error code to return? */
    }
    SSL_free(sock->sslData->ssl);
    sock->sslData->ssl = NULL;
    return APR_SUCCESS;
}

apr_status_t apu_ssl_connect(apr_ssl_socket_t *sock)
{
    int sslOp;

    if (!sock->sslData->ssl)
        return APR_EINVAL;

    if ((sslOp = SSL_connect(sock->sslData->ssl)) == 1) {
        sock->connected = 1;
        return APR_SUCCESS;
    }
    openssl_get_error(sock, sslOp);
    return APR_EGENERAL;
}

apr_status_t apu_ssl_send(apr_ssl_socket_t *sock, const char *buf, 
                          apr_size_t *len)
{
    int sslOp;

    sslOp = SSL_write(sock->sslData->ssl, buf, *len);
    if (sslOp > 0) {
        *len = sslOp;
        return APR_SUCCESS;
    }
    openssl_get_error(sock, sslOp);
    return APR_EGENERAL; /* SSL error? */
}

apr_status_t apu_ssl_recv(apr_ssl_socket_t * sock,
                              char *buf, apr_size_t *len)
{
    int sslOp;

    if (!sock->sslData)
        return APR_EINVAL;

    sslOp = SSL_read(sock->sslData->ssl, buf, *len);
    if (sslOp > 0) {
        *len = sslOp;
        return APR_SUCCESS;
    }
    openssl_get_error(sock, sslOp);
    return APR_EGENERAL; /* SSL error ? */
}

apr_status_t apu_ssl_accept(apr_ssl_socket_t *newSock, 
                            apr_ssl_socket_t *oldSock, apr_pool_t *pool)
{
    apu_ssl_socket_data_t *sslData = apr_pcalloc(pool, sizeof(*sslData));
    apr_os_sock_t fd;
    int sslOp;

    if (!sslData || !oldSock->factory)
        return APR_EINVAL;

    sslData->ssl = SSL_new(oldSock->factory->sslData->ctx);
    if (!sslData->ssl)
        return APR_EINVAL;

    if (apr_os_sock_get(&fd, newSock->plain) != APR_SUCCESS)
        return APR_EINVALSOCK;
    SSL_set_fd(sslData->ssl, fd);

    newSock->pool = pool;
    newSock->sslData = sslData;
    newSock->factory = oldSock->factory;

    if ((sslOp = SSL_accept(sslData->ssl)) != 1) {
        openssl_get_error(newSock, sslOp);
        return APR_EGENERAL;
    }

    return APR_SUCCESS;
}

apr_status_t apu_ssl_raw_error(apr_ssl_socket_t *sock)
{
    if (!sock->sslData)
        return APR_EINVAL;

    if (sock->sslData->sslErr)
        return sock->sslData->sslErr;

    return APR_SUCCESS;
}

#endif
