// SPDX-License-Identifier: Apache-2.0
// Host-only official FRPS fixture. Run within esp-frp's pinned Go module.
package main

import (
	"context"
	"flag"
	"fmt"
	"net"
	"os"
	"os/signal"
	"syscall"
	"time"

	v1 "github.com/fatedier/frp/pkg/config/v1"
	frplog "github.com/fatedier/frp/pkg/util/log"
	"github.com/fatedier/frp/server"
)

func main() {
	port := flag.Int("port", 0, "QEMU-only loopback FRPS port")
	cert := flag.String("cert", "", "temporary test server certificate")
	key := flag.String("key", "", "temporary test server private key")
	flag.Parse()
	if *port < 1024 || *port > 65535 || *cert == "" || *key == "" {
		fmt.Fprintln(os.Stderr, "invalid QEMU FRPS fixture inputs")
		os.Exit(2)
	}
	config := &v1.ServerConfig{BindAddr: "127.0.0.1", BindPort: *port, ProxyBindAddr: "127.0.0.1"}
	config.Auth.Token = "qemu-frps-test-token"
	config.Auth.AdditionalScopes = []v1.AuthScope{v1.AuthScopeHeartBeats, v1.AuthScopeNewWorkConns}
	config.Transport.TLS.Force = true
	config.Transport.TLS.CertFile = *cert
	config.Transport.TLS.KeyFile = *key
	if err := config.Complete(); err != nil {
		panic(err)
	}
	frplog.InitLogger("console", "error", 1, true)
	service, err := server.NewService(config)
	if err != nil {
		panic(err)
	}
	ctx, cancel := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer cancel()
	done := make(chan struct{})
	go func() { service.Run(ctx); close(done) }()
	address := fmt.Sprintf("127.0.0.1:%d", *port)
	ready := false
	for range 100 {
		connection, dialError := net.DialTimeout("tcp4", address, 100*time.Millisecond)
		if dialError == nil {
			_ = connection.Close()
			ready = true
			break
		}
		time.Sleep(20 * time.Millisecond)
	}
	if !ready {
		cancel()
		_ = service.Close()
		<-done
		panic("FRPS did not bind loopback")
	}
	fmt.Printf("QEMU_FRPS_READY port=%d\n", *port)
	<-ctx.Done()
	_ = service.Close()
	<-done
	fmt.Println("QEMU_FRPS_STOPPED")
}
