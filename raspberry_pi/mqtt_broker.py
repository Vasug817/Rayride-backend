import socket
import threading
import sys
import time

PORT = 1883
HOST = '0.0.0.0'

# Dict of subscriptions: topic_filter -> set of client sockets
subscriptions = {}
clients_lock = threading.Lock()

def topic_matches(sub, topic):
    if sub == topic:
        return True
    if sub.endswith('#'):
        prefix = sub[:-1]
        if topic.startswith(prefix):
            return True
    return False

def read_packet(sock):
    try:
        header = sock.recv(1)
        if not header:
            return None
        cmd = header[0]
        
        # Read variable length remaining length
        multiplier = 1
        remaining_length = 0
        while True:
            b = sock.recv(1)
            if not b:
                return None
            byte = b[0]
            remaining_length += (byte & 127) * multiplier
            if (byte & 128) == 0:
                break
            multiplier *= 128
            if multiplier > 128*128*128:
                return None
                
        # Read payload
        payload = bytearray()
        while len(payload) < remaining_length:
            chunk = sock.recv(remaining_length - len(payload))
            if not chunk:
                return None
            payload.extend(chunk)
        return cmd, bytes(payload)
    except Exception:
        return None

def write_var_int(length):
    result = bytearray()
    while True:
        byte = length % 128
        length = length // 128
        if length > 0:
            byte = byte | 128
        result.append(byte)
        if length == 0:
            break
    return result

def handle_client(sock, addr):
    print(f"[Broker] Client connected from {addr[0]}:{addr[1]}")
    client_subs = set()
    try:
        while True:
            res = read_packet(sock)
            if res is None:
                break
            cmd, payload = res
            packet_type = cmd >> 4
            
            # CONNECT (1)
            if packet_type == 1:
                # Respond with CONNACK (0x20, remaining length 2, session present 0x00, return code 0x00)
                sock.sendall(b'\x20\x02\x00\x00')
                print(f"[Broker] CONNECT from {addr[0]}")
                
            # PUBLISH (3)
            elif packet_type == 3:
                # Extract topic
                if len(payload) < 2:
                    continue
                topic_len = (payload[0] << 8) | payload[1]
                topic = payload[2:2+topic_len].decode('utf-8', errors='ignore')
                
                # Check QoS
                qos = (cmd & 0x06) >> 1
                
                # Forward to matching subscribers
                print(f"[Broker] PUBLISH on [{topic}] ({len(payload) - 2 - topic_len} bytes) QoS {qos}")
                
                # Rebuild full packet to forward
                remaining_bytes = payload
                var_len = write_var_int(len(remaining_bytes))
                forward_packet = bytes([cmd]) + var_len + remaining_bytes
                
                with clients_lock:
                    for sub, client_set in list(subscriptions.items()):
                        if topic_matches(sub, topic):
                            for client in list(client_set):
                                if client is not sock:
                                    try:
                                        client.sendall(forward_packet)
                                    except Exception:
                                        # Clean up dead subscriber
                                        client_set.discard(client)
                                        
            # SUBSCRIBE (8)
            elif packet_type == 8:
                if len(payload) < 2:
                    continue
                packet_id = payload[0:2]
                idx = 2
                sub_topics = []
                while idx < len(payload):
                    t_len = (payload[idx] << 8) | payload[idx+1]
                    topic = payload[idx+2:idx+2+t_len].decode('utf-8', errors='ignore')
                    idx += 2 + t_len
                    qos = payload[idx]
                    idx += 1
                    sub_topics.append(topic)
                    
                with clients_lock:
                    for topic in sub_topics:
                        if topic not in subscriptions:
                            subscriptions[topic] = set()
                        subscriptions[topic].add(sock)
                        client_subs.add(topic)
                        print(f"[Broker] {addr[0]} subscribed to [{topic}]")
                        
                # Respond with SUBACK
                # packet type 9, remaining length 2 + len(sub_topics), packet_id, qos replies (0x00 for each)
                rem_len = 2 + len(sub_topics)
                suback = bytes([0x90]) + write_var_int(rem_len) + packet_id + bytes([0x00] * len(sub_topics))
                sock.sendall(suback)
                
            # PINGREQ (12)
            elif packet_type == 12:
                # Respond with PINGRESP (0xD0, remaining length 0)
                sock.sendall(b'\xD0\x00')
                
            # DISCONNECT (14)
            elif packet_type == 14:
                print(f"[Broker] DISCONNECT from {addr[0]}")
                break
    except Exception as e:
        print(f"[Broker] Exception for client {addr[0]}: {e}")
    finally:
        sock.close()
        with clients_lock:
            for topic in client_subs:
                if topic in subscriptions:
                    subscriptions[topic].discard(sock)
        print(f"[Broker] Connection closed for {addr[0]}:{addr[1]}")

def main():
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        server.bind((HOST, PORT))
        server.listen(100)
        print(f"\n=======================================================")
        print(f"  RayGlides Lightweight MQTT Broker Running At:")
        print(f"  {HOST}:{PORT}")
        print(f"=======================================================\n")
    except Exception as e:
        print(f"[Broker] Bind failed: {e}")
        sys.exit(1)
        
    while True:
        try:
            sock, addr = server.accept()
            t = threading.Thread(target=handle_client, args=(sock, addr), daemon=True)
            t.start()
        except KeyboardInterrupt:
            print("\nShutting down broker.")
            break
        except Exception as e:
            print(f"[Broker] Accept exception: {e}")
            time.sleep(0.1)

if __name__ == '__main__':
    main()
