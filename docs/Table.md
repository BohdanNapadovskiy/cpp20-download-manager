| State         | Steps                               | Initiator    | 
|---------------|-------------------------------------|--------------|
| `--`          | Queued                              | User         |
| `Queued`      | Downloading, Paused, Canceled       | Worker, User, User |
| `Downloading` | Completed, Failed, Retrying, Paused, Canceled | Worker, Worker, Worker, User, User       |
| `Retrying`    | Downloading, Paused, Canceled, Queued | Worker, User, User, Worker (shutdown) |
| `Failed`      | Queued                              | User         |
| `Paused`      | Canceled ,Queued                    | User, User         |
| `Canceled`    |                                     |          |
| `Completed`   |                                     |        |
