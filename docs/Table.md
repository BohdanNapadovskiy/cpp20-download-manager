| State         | Steps                               | Initiator    | 
|---------------|-------------------------------------|--------------|
| `--`          | Queued                              | User         |
| `Queued`      | Downloading, Paused, Canceled       | Worker, User, User |
| `Downloading` | Completed, Failed, Paused, Canceled | Worker, Worker, User, User       |
| `Failed`      | Queued                              | User         |
| `Paused`      | Canceled ,Queued                    | User, User         |
| `Canceled`    |                                     |          |
| `Completed`   |                                     |        |